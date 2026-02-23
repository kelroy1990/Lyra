/*
 * codec_opus.c — Opus audio decoder (Ogg container)
 *
 * Uses libopus (opus_decode) with a minimal inline Ogg page demuxer.
 * Supports single-bitstream .opus files.  Output is always 48 kHz stereo.
 *
 * Output: int32_t stereo interleaved, left-justified (16-bit PCM << 16).
 *
 * ReplayGain: R128_TRACK_GAIN (int16 Q7.8) from OpusTags → info.gain_db
 * Total frames: scanned from final Ogg page granule at open time.
 * Seek: linear interpolation of byte offset + Ogg forward-sync.
 */

#include "audio_codecs_internal.h"
#include <opus.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "codec_opus";

/* Opus always decodes to 48 kHz.  Max frame = 120 ms = 5760 samples/ch. */
#define OPUS_MAX_FRAME_SZ  5760
/* Ogg packet buffer: 8 KB covers any realistic Opus packet                */
#define OGG_PKT_BUF_SZ     8192

/* -----------------------------------------------------------------------
 * Decoder state
 * ----------------------------------------------------------------------- */

typedef struct {
    /* Ogg page / packet state */
    uint8_t  seg_table[255];
    uint32_t num_segs;          /* segments in current page        */
    uint32_t seg_idx;           /* next segment to read            */
    uint32_t serial_number;     /* selected logical bitstream      */
    bool     serial_set;        /* true once serial_number is set  */

    /* Stored so rewind can re-seek to the first audio packet */
    long     first_audio_offset; /* file offset after OpusTags page */
    long     file_size;          /* total file size (for seek estimation) */

    /* Pre-skip (from OpusHead) — samples to discard at stream start */
    int32_t  pre_skip;

    /* ReplayGain: R128_TRACK_GAIN from OpusTags (0.0 = no tag) */
    float    gain_db;

    /* Packet reassembly scratch */
    uint8_t  pkt_buf[OGG_PKT_BUF_SZ];

    /* Opus decoder */
    OpusDecoder *dec;
    int          channels;

    /* PCM output scratch (heap) */
    int16_t *pcm_scratch;        /* OPUS_MAX_FRAME_SZ * channels */
} opus_state_t;

/* -----------------------------------------------------------------------
 * Minimal Ogg reader
 * ----------------------------------------------------------------------- */

static uint32_t rd_le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int64_t rd_le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return (int64_t)v;
}

/*
 * Read the next Ogg page header from FILE.
 * The file must be positioned at the start of a page (i.e. at "OggS").
 * If the first 4 bytes are not "OggS", fall back to a forward scan.
 *
 * Fills st->seg_table, st->num_segs, st->seg_idx = 0.
 * Returns true on success (file now positioned at start of page body).
 */
static bool ogg_read_page(FILE *f, opus_state_t *st, int8_t *htype_out,
                          int64_t *gran_out)
{
    uint8_t hdr[27]; /* full page header (including "OggS") */

    /* Try direct read — works for sequential access */
    if (fread(hdr, 1, 4, f) != 4) return false;

    if (memcmp(hdr, "OggS", 4) != 0) {
        /* Desync: scan forward byte-by-byte for "OggS" */
        uint8_t w[4] = { hdr[0], hdr[1], hdr[2], hdr[3] };
        bool found = false;
        for (int guard = 0; guard < (1 << 20); guard++) {
            int c = fgetc(f);
            if (c == EOF) return false;
            w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = (uint8_t)c;
            if (w[0]=='O' && w[1]=='g' && w[2]=='g' && w[3]=='S') {
                found = true;
                break;
            }
        }
        if (!found) return false;
        /* hdr[0..3] = "OggS" already conceptually consumed; read remaining 23 */
        if (fread(hdr + 4, 1, 23, f) != 23) return false;
    } else {
        /* Direct match: read remaining 23 bytes */
        if (fread(hdr + 4, 1, 23, f) != 23) return false;
    }

    /* hdr layout (after "OggS"):
     *   [4]  version        (must be 0)
     *   [5]  header_type
     *   [6..13]  granule_position (int64 LE)
     *   [14..17] serial_number   (uint32 LE)
     *   [18..21] page_seq_no     (uint32 LE)
     *   [22..25] CRC             (uint32 LE)
     *   [26]     num_segments
     */
    if (hdr[4] != 0) return false; /* version mismatch */

    if (htype_out) *htype_out = (int8_t)hdr[5];
    if (gran_out)  *gran_out  = rd_le64(hdr + 6);

    uint32_t sn = rd_le32(hdr + 14);

    /* Track serial number — use the first one we see */
    if (!st->serial_set) {
        st->serial_number = sn;
        st->serial_set    = true;
    } else if (sn != st->serial_number) {
        /* Different logical stream — skip its body */
        uint8_t n = hdr[26];
        uint8_t segtab[255];
        uint32_t body = 0;
        if (n && fread(segtab, 1, n, f) == n)
            for (uint8_t i = 0; i < n; i++) body += segtab[i];
        fseek(f, (long)body, SEEK_CUR);
        return ogg_read_page(f, st, htype_out, gran_out); /* recurse */
    }

    st->num_segs = hdr[26];
    st->seg_idx  = 0;
    if (st->num_segs && fread(st->seg_table, 1, st->num_segs, f) != st->num_segs)
        return false;

    return true; /* file now at page body start */
}

/*
 * Read the next complete Ogg packet into buf (max buf_max bytes).
 * Automatically advances to new pages as needed.
 * Returns packet length (>=0) on success, 0 at end-of-stream, -1 on error.
 */
static int ogg_next_packet(FILE *f, opus_state_t *st, uint8_t *buf, uint32_t buf_max)
{
    uint32_t pkt_len = 0;

    for (;;) {
        /* Need a new page? */
        if (st->seg_idx >= st->num_segs) {
            if (!ogg_read_page(f, st, NULL, NULL)) {
                return (pkt_len > 0) ? (int)pkt_len : 0;
            }
        }

        uint32_t seg = st->seg_table[st->seg_idx++];

        if (seg > 0) {
            if (pkt_len + seg > buf_max) {
                /* Packet overflows our buffer — skip the rest by seeking */
                fseek(f, (long)seg, SEEK_CUR);
                /* Drain remaining segments of this (truncated) packet */
                while (seg == 255 && st->seg_idx < st->num_segs) {
                    seg = st->seg_table[st->seg_idx++];
                    fseek(f, (long)seg, SEEK_CUR);
                }
                ESP_LOGW(TAG, "Ogg packet too large (>%u B), skipped", buf_max);
                return -1;
            }
            if (fread(buf + pkt_len, 1, seg, f) != seg) return -1;
            pkt_len += seg;
        }

        if (seg < 255) {
            /* End of packet (lacing < 255 terminates) */
            return (int)pkt_len;
        }
        /* seg == 255 → packet continues in next segment */
    }
}

/* -----------------------------------------------------------------------
 * Vtable: decode
 * ----------------------------------------------------------------------- */

static int32_t opus_decode_fn(codec_handle_t *h, int32_t *buffer,
                               uint32_t max_frames)
{
    opus_state_t *st = (opus_state_t *)h->opus.state;
    if (!st) return -1;

    for (;;) {
        int pkt_len = ogg_next_packet(h->file, st, st->pkt_buf, OGG_PKT_BUF_SZ);
        if (pkt_len == 0) return 0;   /* EOF */
        if (pkt_len < 0)  continue;   /* oversized packet — already logged */

        /* Opus header packets start with a magic string — skip them */
        if (pkt_len >= 8 && (memcmp(st->pkt_buf, "OpusHead", 8) == 0 ||
                             memcmp(st->pkt_buf, "OpusTags", 8) == 0)) {
            continue;
        }

        int frames = opus_decode(st->dec, st->pkt_buf, pkt_len,
                                 st->pcm_scratch, OPUS_MAX_FRAME_SZ, 0);
        if (frames < 0) {
            /* Silently skip decode errors (e.g. from seek artefacts) */
            continue;
        }

        /* Apply pre-skip (drop leading samples from the first packet) */
        int32_t start = 0;
        if (st->pre_skip > 0) {
            int32_t skip = (st->pre_skip < frames) ? st->pre_skip : frames;
            start         = skip;
            st->pre_skip -= skip;
        }

        uint32_t actual = (uint32_t)(frames - start);
        if (actual > max_frames) actual = max_frames;
        if (actual == 0) continue;

        /* Convert int16_t → int32_t left-justified */
        const int16_t *src = st->pcm_scratch + (size_t)start * (size_t)st->channels;
        if (st->channels == 1) {
            for (uint32_t i = 0; i < actual; i++) {
                int32_t s = (int32_t)src[i] << 16;
                buffer[i * 2]     = s;
                buffer[i * 2 + 1] = s;
            }
        } else {
            uint32_t n = actual * 2;
            for (uint32_t i = 0; i < n; i++)
                buffer[i] = (int32_t)src[i] << 16;
        }

        return (int32_t)actual;
    }
}

/* -----------------------------------------------------------------------
 * Vtable: seek
 *
 * frame_pos == 0  → exact rewind to first audio page.
 * frame_pos  > 0  → linear-interpolation estimate of byte offset,
 *                   then forward-scan for the next OggS page boundary.
 *                   Requires total_frames to have been determined at open.
 * ----------------------------------------------------------------------- */

static bool opus_seek_fn(codec_handle_t *h, uint64_t frame_pos)
{
    opus_state_t *st = (opus_state_t *)h->opus.state;
    if (!st) return false;

    /* Common reset — ogg_read_page will re-detect serial_number */
    st->num_segs   = 0;
    st->seg_idx    = 0;
    st->serial_set = false;
    st->pre_skip   = 0;  /* pre-skip already applied during original open */

    if (frame_pos == 0) {
        fseek(h->file, st->first_audio_offset, SEEK_SET);
        return true;
    }

    /* Approximate seek: linear interpolation of byte offset */
    if (h->info.total_frames == 0 || st->file_size <= (long)st->first_audio_offset)
        return false;

    long audio_span = st->file_size - (long)st->first_audio_offset;
    long est = (long)st->first_audio_offset
             + (long)((double)frame_pos * (double)audio_span
                      / (double)h->info.total_frames);

    if (est < (long)st->first_audio_offset) est = (long)st->first_audio_offset;
    if (est >= st->file_size - 1)           est = st->file_size - 2;

    fseek(h->file, est, SEEK_SET);
    /* ogg_read_page's forward-scan will find the next OggS boundary */
    return true;
}

/* -----------------------------------------------------------------------
 * Vtable: close
 * ----------------------------------------------------------------------- */

static void opus_close_fn(codec_handle_t *h)
{
    opus_state_t *st = (opus_state_t *)h->opus.state;
    if (st) {
        if (st->dec) opus_decoder_destroy(st->dec);
        free(st->pcm_scratch);
        free(st);
        h->opus.state = NULL;
    }
}

static const codec_vtable_t opus_vtable = {
    .decode = opus_decode_fn,
    .seek   = opus_seek_fn,
    .close  = opus_close_fn,
};

/* -----------------------------------------------------------------------
 * codec_opus_open — parse OpusHead, read OpusTags for R128_TRACK_GAIN,
 *                   scan final page for total_frames, init decoder
 * ----------------------------------------------------------------------- */

bool codec_opus_open(codec_handle_t *h)
{
    opus_state_t *st = calloc(1, sizeof(opus_state_t));
    if (!st) return false;

    /* --- Read first Ogg page (BOS, contains OpusHead) --- */
    int8_t htype;
    if (!ogg_read_page(h->file, st, &htype, NULL)) {
        ESP_LOGE(TAG, "Failed to read first Ogg page");
        free(st);
        return false;
    }

    uint8_t head[64];
    int head_len = ogg_next_packet(h->file, st, head, sizeof(head));
    if (head_len < 19 || memcmp(head, "OpusHead", 8) != 0) {
        ESP_LOGE(TAG, "OpusHead not found (len=%d)", head_len);
        free(st);
        return false;
    }

    /* OpusHead layout (v1):
     *   [0..7]  magic "OpusHead"
     *   [8]     version  (must be 1)
     *   [9]     channel count
     *   [10..11] pre-skip (uint16 LE)
     *   [12..15] input sample rate (uint32 LE, informational)
     *   [16..17] output gain (int16 LE, Q7.8 dB — applied automatically by libopus)
     *   [18]    channel mapping family
     */
    st->channels  = head[9];
    st->pre_skip  = (int32_t)((uint16_t)head[10] | ((uint16_t)head[11] << 8));

    /* --- Read OpusTags and extract R128_TRACK_GAIN --- */
    /*
     * pkt_buf (8 KB) fits most tags.  ogg_next_packet returns -1 on overflow
     * (e.g. embedded cover art) — in that case gain_db stays 0.0.
     */
    {
        int tags_len = ogg_next_packet(h->file, st, st->pkt_buf, OGG_PKT_BUF_SZ);
        if (tags_len >= 16 && memcmp(st->pkt_buf, "OpusTags", 8) == 0) {
            /*
             * Vorbis comment format (RFC 7845 §5.2):
             *   [8..11]  vendor_length (uint32 LE)
             *   [12..]   vendor_string (vendor_length bytes)
             *   [next]   comment_count (uint32 LE)
             *   [next]   for each: length (uint32 LE) + "KEY=VALUE" string
             */
            const uint8_t *p   = st->pkt_buf + 8;
            const uint8_t *end = st->pkt_buf + tags_len;

            /* Skip vendor string */
            if (p + 4 <= end) {
                uint32_t vlen = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                              | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                p += 4;
                if (p + vlen <= end) p += vlen; else p = end;
            }

            /* Iterate comment list */
            if (p + 4 <= end) {
                uint32_t nc = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                            | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                p += 4;
                for (uint32_t ci = 0; ci < nc && p + 4 <= end; ci++) {
                    uint32_t clen = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                                  | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    p += 4;
                    if (clen > (uint32_t)(end - p)) break;
                    /* R128_TRACK_GAIN is int16 Q7.8 (units: 1/256 dB) */
                    if (clen >= 16 &&
                        strncasecmp((const char *)p, "R128_TRACK_GAIN=", 16) == 0) {
                        int raw = atoi((const char *)p + 16);
                        st->gain_db = (float)(int16_t)raw / 256.0f;
                    }
                    p += clen;
                }
            }
        }
        /* If tags_len < 0, ogg_next_packet already consumed and skipped the packet */
    }

    /* Record file offset: audio packets start here */
    st->first_audio_offset = ftell(h->file);
    /* Reset segment state so ogg_next_packet fetches the next page cleanly */
    st->num_segs = 0;
    st->seg_idx  = 0;

    /* --- Scan last ~64 KB of file to find final granule → total_frames --- */
    {
        fseek(h->file, 0, SEEK_END);
        long fsize = ftell(h->file);
        st->file_size = fsize;

        long scan_from = fsize - 65536;
        if (scan_from < st->first_audio_offset)
            scan_from = st->first_audio_offset;
        fseek(h->file, scan_from, SEEK_SET);

        /*
         * We know serial_number from the BOS page (set during ogg_read_page above,
         * before we reset serial_set below).  Use it to filter pages here.
         */
        uint32_t target_sn  = st->serial_number;
        int64_t  last_gran  = -1;
        uint8_t  phdr[27];
        uint8_t  segtab[255];

        while (ftell(h->file) + 27 <= fsize) {
            if (fread(phdr, 1, 4, h->file) != 4) break;
            if (memcmp(phdr, "OggS", 4) != 0) {
                fseek(h->file, -3, SEEK_CUR);
                continue;
            }
            if (fread(phdr + 4, 1, 23, h->file) != 23) break;
            int64_t  gran = rd_le64(phdr + 6);
            uint32_t sn   = rd_le32(phdr + 14);
            uint8_t  ns   = phdr[26];
            if (ns && fread(segtab, 1, ns, h->file) != ns) break;
            uint32_t body = 0;
            for (uint8_t i = 0; i < ns; i++) body += segtab[i];
            if (sn == target_sn && gran > last_gran) last_gran = gran;
            fseek(h->file, (long)body, SEEK_CUR);
        }

        if (last_gran > st->pre_skip) {
            h->info.total_frames = (uint64_t)(last_gran - st->pre_skip);
            h->info.duration_ms  =
                (uint32_t)(h->info.total_frames * 1000ULL / 48000ULL);
        }

        fseek(h->file, st->first_audio_offset, SEEK_SET);
    }

    st->serial_set = false; /* Will re-detect on first audio page */

    /* --- Create Opus decoder (always 48 kHz) --- */
    int err;
    st->dec = opus_decoder_create(48000, st->channels, &err);
    if (!st->dec) {
        ESP_LOGE(TAG, "opus_decoder_create: %s", opus_strerror(err));
        free(st);
        return false;
    }

    st->pcm_scratch = malloc((size_t)OPUS_MAX_FRAME_SZ * (size_t)st->channels
                              * sizeof(int16_t));
    if (!st->pcm_scratch) {
        opus_decoder_destroy(st->dec);
        free(st);
        return false;
    }

    h->opus.state           = st;
    h->info.sample_rate     = 48000;   /* Opus always outputs 48 kHz */
    h->info.bits_per_sample = 16;
    h->info.channels        = (uint8_t)st->channels;
    h->info.gain_db         = st->gain_db;
    h->vt                   = &opus_vtable;

    ESP_LOGI(TAG, "Opus: %d-ch, pre_skip=%ld, gain=%.2f dB, frames=%llu",
             st->channels, (long)st->pre_skip, st->gain_db,
             (unsigned long long)h->info.total_frames);
    return true;
}
