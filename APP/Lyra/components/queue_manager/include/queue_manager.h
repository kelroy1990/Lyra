#ifndef QUEUE_MANAGER_H
#define QUEUE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Track source types
//--------------------------------------------------------------------+

typedef enum {
    QM_SOURCE_SD,       // Local SD file — plays via sd_player
    QM_SOURCE_SUBSONIC, // Subsonic track — builds URL, plays via net_audio
    QM_SOURCE_NET,      // Direct URL — plays via net_audio
} qm_source_t;

//--------------------------------------------------------------------+
// Queue track entry
//--------------------------------------------------------------------+

typedef struct {
    qm_source_t source;
    char   title[128];
    char   artist[64];
    char   album[128];
    uint32_t duration_ms;
    union {
        char file_path[256];      // QM_SOURCE_SD
        char subsonic_id[48];     // QM_SOURCE_SUBSONIC
        char url[256];            // QM_SOURCE_NET
    };
} qm_track_t;

#define QM_MAX_TRACKS  128

//--------------------------------------------------------------------+
// Repeat mode (reuse sd_player's enum values)
//--------------------------------------------------------------------+

typedef enum {
    QM_REPEAT_OFF = 0,
    QM_REPEAT_ONE,
    QM_REPEAT_ALL,
} qm_repeat_t;

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void     qm_init(void);

// Enqueue
void     qm_append(const qm_track_t *track);
void     qm_insert_next(const qm_track_t *track);
void     qm_append_batch(const qm_track_t *tracks, int count);

// Control
void     qm_play(void);              // Start playing from current position
void     qm_next(void);
void     qm_prev(void);
void     qm_jump(int index);
void     qm_stop(void);              // Stop and deactivate queue
void     qm_clear(void);
void     qm_remove(int index);

// State
int      qm_count(void);
int      qm_current_index(void);
bool     qm_is_active(void);
const qm_track_t *qm_get_track(int index);
bool     qm_get_shuffle(void);
qm_repeat_t qm_get_repeat(void);

// Shuffle / repeat
void     qm_set_shuffle(bool enabled);
void     qm_set_repeat(qm_repeat_t mode);

// Notifications — called by audio EOF callbacks
void     qm_notify_track_ended(void);
void     qm_notify_track_error(const char *reason);

// CDC command handler
typedef void (*qm_print_fn_t)(const char *fmt, ...);
void     qm_handle_cdc_command(const char *sub, qm_print_fn_t print);

#ifdef __cplusplus
}
#endif

#endif /* QUEUE_MANAGER_H */
