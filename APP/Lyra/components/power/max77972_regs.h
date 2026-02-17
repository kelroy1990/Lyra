/**
 * @file    max77972_regs.h
 * @brief   MAX77972 register addresses, bitfield masks, and conversion macros
 *
 * MAX77972EWX+: 3-in-1 Li-ion charger + ModelGauge m5 EZ fuel gauge + USB-C detection
 *
 * I2C Addressing:
 *   - 0x36 (7-bit): FG_FUNC_MAP — fuel gauge + charger registers (0x00–0xFF)
 *   - 0x37 (7-bit): FG_DEBUG_MAP — NV config registers (byte addr = internal_addr - 0x100)
 *   - All registers are 16-bit, LSB-first on wire
 *
 * Datasheet: MAX77972 Rev 0, Analog Devices
 */
#pragma once

// ---------------------------------------------------------------------------
// I2C device addresses (7-bit)
// ---------------------------------------------------------------------------
#define MAX77972_ADDR_MAIN      0x36    // FG_FUNC_MAP: fuel gauge + charger
#define MAX77972_ADDR_NV        0x37    // FG_DEBUG_MAP: NV config registers

// ---------------------------------------------------------------------------
// Fuel Gauge registers (read via ADDR_MAIN = 0x36)
// ---------------------------------------------------------------------------
#define MAX77972_REG_STATUS         0x00    // Alert/status flags
#define MAX77972_REG_CHG_MASK_STS   0x01    // Charger interrupt mask + status
#define MAX77972_REG_VALRT_TH       0x03    // Voltage alert thresholds
#define MAX77972_REG_TALRT_TH       0x04    // Temperature alert thresholds
#define MAX77972_REG_SALRT_TH       0x05    // SOC alert thresholds
#define MAX77972_REG_REP_CAP        0x06    // Reported remaining capacity
#define MAX77972_REG_REP_SOC        0x07    // Reported state of charge
#define MAX77972_REG_ID_VOLT        0x08    // ID voltage
#define MAX77972_REG_MAX_MIN_TEMP   0x09    // Max/Min temperature
#define MAX77972_REG_MAX_MIN_CURR   0x0A    // Max/Min current
#define MAX77972_REG_MAX_MIN_VOLT   0x0B    // Max/Min voltage
#define MAX77972_REG_CONFIG         0x0C    // Configuration
#define MAX77972_REG_MIX_SOC        0x0D    // Mixed SOC
#define MAX77972_REG_AV_SOC         0x0E    // Average SOC
#define MAX77972_REG_MISC_CFG       0x0F    // Miscellaneous config
#define MAX77972_REG_FULL_CAP_REP   0x10    // Full capacity (reported)
#define MAX77972_REG_AGE            0x16    // Cell age
#define MAX77972_REG_CYCLES         0x17    // Charge cycles
#define MAX77972_REG_DESIGN_CAP     0x18    // Design capacity
#define MAX77972_REG_AVG_VCELL      0x19    // Average cell voltage
#define MAX77972_REG_VCELL          0x1A    // Instantaneous cell voltage
#define MAX77972_REG_TEMP           0x1B    // Temperature
#define MAX77972_REG_CURRENT        0x1C    // Instantaneous current
#define MAX77972_REG_AVG_CURRENT    0x1D    // Average current
#define MAX77972_REG_VEMPTY         0x1F    // Empty voltage threshold
#define MAX77972_REG_DEV_NAME       0x21    // Device name
#define MAX77972_REG_FULL_CAP_NOM   0x23    // Full capacity (nominal)
#define MAX77972_REG_FULL_CAP       0x24    // Full capacity
#define MAX77972_REG_VF_REM_CAP     0x25    // VF remaining capacity
#define MAX77972_REG_MIX_CAP        0x26    // Mixed capacity
#define MAX77972_REG_AV_CAP         0x27    // Average capacity
#define MAX77972_REG_CHARGING_CURR  0x28    // Real-time charging current (read-only)
#define MAX77972_REG_ICHG_TERM      0x29    // Charge termination current
#define MAX77972_REG_CHARGING_VOLT  0x2A    // Real-time charging voltage (read-only)
#define MAX77972_REG_DIE_TEMP       0x34    // Die temperature
#define MAX77972_REG_AVG_TA         0x35    // Average temperature
#define MAX77972_REG_SOC_HOLD       0x39    // SOC hold config
#define MAX77972_REG_PROT_STATUS    0x3A    // Protection status
#define MAX77972_REG_FSTAT          0x3D    // Fuel gauge status
#define MAX77972_REG_FPROT_STAT     0x3E    // FG protection status
#define MAX77972_REG_TIMER          0x3F    // Timer
#define MAX77972_REG_DQACC          0x45    // Charge accumulator
#define MAX77972_REG_DPACC          0x46    // Power accumulator
#define MAX77972_REG_QH             0x4D    // Coulomb counter
#define MAX77972_REG_ICHGIN         0x51    // CHGIN current
#define MAX77972_REG_VSYS           0x52    // System voltage
#define MAX77972_REG_MODEL_CFG      0xA3    // Model configuration
#define MAX77972_REG_TIMER_H        0xBE    // Timer (high)
#define MAX77972_REG_TTE            0x11    // Time to empty
#define MAX77972_REG_TTF            0x20    // Time to full

// ---------------------------------------------------------------------------
// Charger config registers (read/write via ADDR_MAIN = 0x36)
// ---------------------------------------------------------------------------
#define MAX77972_REG_NCHGCFG0       0xD0    // SPS mode, watchdog, fast-charge timer
#define MAX77972_REG_NCHGCFG1       0xD1    // MINVSYS, B2SOVRC, OTG_ILIM
#define MAX77972_REG_NCHGCFG2       0xD2    // REGTEMP, FSW, FSHIP, WDTCLR
#define MAX77972_REG_NCHGCFG3       0xD3    // CHGIN_ILIM, VBYPSET
#define MAX77972_REG_NCHGCFG4       0xD4    // USB detection, CHGDETEN
#define MAX77972_REG_NCHGCFG5       0xD5    // ChgEnable, DeepShip, RestartChg
#define MAX77972_REG_CHG_DTLS_00    0xD6    // Charger status (real-time)
#define MAX77972_REG_CHG_DTLS_01    0xD7    // Charger details (CHG_DTLS, BAT_DTLS)
#define MAX77972_REG_USB_DTLS       0xD8    // USB detection details
#define MAX77972_REG_VBYP           0xDB    // Bypass voltage
#define MAX77972_REG_COMMAND        0xE0    // Command register
#define MAX77972_REG_USR            0xE1    // User register (NLOCK)
#define MAX77972_REG_VFOCV          0xFB    // VF OCV

// ---------------------------------------------------------------------------
// NV registers (via ADDR_NV = 0x37, byte_addr = internal_addr - 0x100)
// Write requires NLOCK unlock (write 0x0059 to USR, then lock with 0x0000)
// ---------------------------------------------------------------------------
#define MAX77972_NV_FILTER_CFG      0x9D    // int 0x19D: Filter config
#define MAX77972_NV_RCOMP0          0xA6    // int 0x1A6: RComp0
#define MAX77972_NV_TEMPCO          0xA7    // int 0x1A7: TempCo
#define MAX77972_NV_RGAIN           0xAB    // int 0x1AB: RGain
#define MAX77972_NV_RELAX_CFG       0xB6    // int 0x1B6: Relax config
#define MAX77972_NV_CONVG_CFG       0xB7    // int 0x1B7: Convergence config
#define MAX77972_NV_NVCFG2          0xBA    // int 0x1BA: NV config 2 (enSC)
#define MAX77972_NV_HIB_CFG         0xBB    // int 0x1BB: Hibernate config
#define MAX77972_NV_CHG_CFG0        0xC2    // int 0x1C2: Step charge mode
#define MAX77972_NV_STEP_CURR       0xC4    // int 0x1C4: Step charge currents
#define MAX77972_NV_STEP_VOLT       0xC5    // int 0x1C5: Step charge voltages
#define MAX77972_NV_ALRT_CFG        0xC7    // int 0x1C7: Alert config
#define MAX77972_NV_CGAIN           0xC8    // int 0x1C8: Current gain/offset
#define MAX77972_NV_ADC_CFG         0xC9    // int 0x1C9: ADC config
#define MAX77972_NV_THERM_CFG       0xCA    // int 0x1CA: Thermistor config
#define MAX77972_NV_VCHG_CFG1       0xCC    // int 0x1CC: Room/Warm/Cool charge voltage
#define MAX77972_NV_VCHG_CFG2       0xCD    // int 0x1CD: Hot2/Hot1/Cold1/Cold2 charge voltage
#define MAX77972_NV_ICHG_CFG1       0xCE    // int 0x1CE: Room/Warm/Cool charge current
#define MAX77972_NV_ICHG_CFG2       0xCF    // int 0x1CF: Hot2/Hot1/Cold1/Cold2 charge current
#define MAX77972_NV_TPRT_TH1        0xD1    // int 0x1D1: JEITA temps (Tcold2/Tcold1/Tcool/Troom)
#define MAX77972_NV_TPRT_TH2        0xD5    // int 0x1D5: JEITA temps (Ttoohot/Thot2/Thot1/Twarm)
#define MAX77972_NV_PROT_MISC_TH    0xD6    // int 0x1D6: Protection misc threshold
#define MAX77972_NV_PROT_CFG        0xD7    // int 0x1D7: Protection config
#define MAX77972_NV_DELAY_CFG       0xDC    // int 0x1DC: Delay config
#define MAX77972_NV_SC_OCV_LIM      0xE1    // int 0x1E1: Step charge OCV limit
#define MAX77972_NV_DESIGN_VOLT     0xE3    // int 0x1E3: Design voltage

// ---------------------------------------------------------------------------
// Status register (0x00) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_STATUS_POR          (1 << 1)    // Power-On Reset
#define MAX77972_STATUS_IMN          (1 << 2)    // Current minimum alert
#define MAX77972_STATUS_BST          (1 << 3)    // Battery status
#define MAX77972_STATUS_IMX          (1 << 6)    // Current maximum alert
#define MAX77972_STATUS_DSOCI        (1 << 7)    // dSOC interrupt
#define MAX77972_STATUS_VMN          (1 << 8)    // Voltage minimum alert
#define MAX77972_STATUS_TMN          (1 << 9)    // Temperature minimum alert
#define MAX77972_STATUS_SMN          (1 << 10)   // SOC minimum alert
#define MAX77972_STATUS_BI           (1 << 11)   // Battery insertion
#define MAX77972_STATUS_VMX          (1 << 12)   // Voltage maximum alert
#define MAX77972_STATUS_TMX          (1 << 13)   // Temperature maximum alert
#define MAX77972_STATUS_SMX          (1 << 14)   // SOC maximum alert
#define MAX77972_STATUS_BR           (1 << 15)   // Battery removal

// ---------------------------------------------------------------------------
// ChgMaskSts register (0x01) bitfields
// [15:8] = Masks (M), [7:0] = Interrupt flags (I)
// ---------------------------------------------------------------------------
#define MAX77972_CHGMASK_BYP_M       (1 << 9)
#define MAX77972_CHGMASK_BAT_M       (1 << 11)
#define MAX77972_CHGMASK_CHG_M       (1 << 12)
#define MAX77972_CHGMASK_CHGIN_M     (1 << 14)
#define MAX77972_CHGMASK_AICL_M      (1 << 15)
#define MAX77972_CHGMASK_BYP_I       (1 << 1)
#define MAX77972_CHGMASK_BAT_I       (1 << 3)
#define MAX77972_CHGMASK_CHG_I       (1 << 4)
#define MAX77972_CHGMASK_CHGIN_I     (1 << 6)
#define MAX77972_CHGMASK_AICL_I      (1 << 7)

// ---------------------------------------------------------------------------
// Config register (0x0C) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CONFIG_BER           (1 << 0)    // Battery empty response
#define MAX77972_CONFIG_BEI           (1 << 1)    // Battery empty interrupt
#define MAX77972_CONFIG_AEN           (1 << 2)    // Alert enable
#define MAX77972_CONFIG_FTHRM         (1 << 3)    // Force thermistor
#define MAX77972_CONFIG_TEX           (1 << 8)    // Temperature external
#define MAX77972_CONFIG_IS            (1 << 11)   // Current alert sticky
#define MAX77972_CONFIG_VS            (1 << 12)   // Voltage alert sticky
#define MAX77972_CONFIG_TS            (1 << 13)   // Temperature alert sticky
#define MAX77972_CONFIG_SS            (1 << 14)   // SOC alert sticky

// ---------------------------------------------------------------------------
// nChgConfig0 (0xD0) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CHGCFG0_MODE_MASK      0x000F
#define MAX77972_CHGCFG0_MODE_BAT_ONLY  0x00    // Battery only, buck off
#define MAX77972_CHGCFG0_MODE_CHG_OFF   0x04    // Charger off, buck on
#define MAX77972_CHGCFG0_MODE_CHG_ON    0x05    // Charger on (JEITA/step)
#define MAX77972_CHGCFG0_MODE_REV_BST   0x08    // Reverse boost to BYP
#define MAX77972_CHGCFG0_MODE_OTG       0x0A    // OTG to CHGIN
#define MAX77972_CHGCFG0_WDTEN          (1 << 4)
#define MAX77972_CHGCFG0_DISIBS         (1 << 6)
#define MAX77972_CHGCFG0_FCHGTIME_SHIFT 8
#define MAX77972_CHGCFG0_FCHGTIME_MASK  (0x07 << 8)
#define MAX77972_CHGCFG0_RECYCLE_EN     (1 << 11)
#define MAX77972_CHGCFG0_PQEN           (1 << 15)
#define MAX77972_CHGCFG0_LSEL           (1 << 14)

// ---------------------------------------------------------------------------
// nChgConfig1 (0xD1) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CHGCFG1_OTG_ILIM_SHIFT 6
#define MAX77972_CHGCFG1_OTG_ILIM_MASK  (0x03 << 6)
#define MAX77972_CHGCFG1_B2SOVRC_SHIFT  8
#define MAX77972_CHGCFG1_B2SOVRC_MASK   (0x0F << 8)
#define MAX77972_CHGCFG1_MINVSYS_SHIFT  12
#define MAX77972_CHGCFG1_MINVSYS_MASK   (0x07 << 12)

// ---------------------------------------------------------------------------
// nChgConfig2 (0xD2) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CHGCFG2_WDTCLR_MASK    0x0003
#define MAX77972_CHGCFG2_WDTCLR_CLEAR   0x0001
#define MAX77972_CHGCFG2_DIS_AICL       (1 << 4)
#define MAX77972_CHGCFG2_SLOWLX_SHIFT   5
#define MAX77972_CHGCFG2_SLOWLX_MASK    (0x03 << 5)
#define MAX77972_CHGCFG2_B2SOVRC_DTC    (1 << 7)
#define MAX77972_CHGCFG2_FSHIP_MODE     (1 << 8)
#define MAX77972_CHGCFG2_FSW_SHIFT      9
#define MAX77972_CHGCFG2_FSW_MASK       (0x03 << 9)
#define MAX77972_CHGCFG2_REGTEMP_SHIFT  11
#define MAX77972_CHGCFG2_REGTEMP_MASK   (0x0F << 11)
#define MAX77972_CHGCFG2_WD_QBATOFF     (1 << 15)

// ---------------------------------------------------------------------------
// nChgConfig3 (0xD3) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CHGCFG3_CHGIN_ILIM_SHIFT  0
#define MAX77972_CHGCFG3_CHGIN_ILIM_MASK   0x007F
#define MAX77972_CHGCFG3_VBYPSET_SHIFT     8
#define MAX77972_CHGCFG3_VBYPSET_MASK      (0x7F << 8)

// ---------------------------------------------------------------------------
// nChgConfig5 (0xD5) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CHGCFG5_CCDETEN        (1 << 0)
#define MAX77972_CHGCFG5_CHG_ENABLE     (1 << 1)
#define MAX77972_CHGCFG5_DEEP_SHIP      (1 << 2)
#define MAX77972_CHGCFG5_RESTART_CHG    (1 << 3)

// ---------------------------------------------------------------------------
// ChgDetails00 (0xD6) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CHGDTLS00_CHGEN        (1 << 0)
#define MAX77972_CHGDTLS00_CHGIN_DTLS_SHIFT  5
#define MAX77972_CHGDTLS00_CHGIN_DTLS_MASK   (0x03 << 5)
#define MAX77972_CHGDTLS00_BYP_OK       (1 << 8)
#define MAX77972_CHGDTLS00_BAT_OK       (1 << 11)
#define MAX77972_CHGDTLS00_CHG_OK       (1 << 12)
#define MAX77972_CHGDTLS00_CHGIN_OK     (1 << 14)
#define MAX77972_CHGDTLS00_AICL_OK      (1 << 15)

// ---------------------------------------------------------------------------
// ChgDetails01 (0xD7) bitfields
// ---------------------------------------------------------------------------
#define MAX77972_CHGDTLS01_BYP_DTLS_SHIFT   0
#define MAX77972_CHGDTLS01_BYP_DTLS_MASK    0x000F
#define MAX77972_CHGDTLS01_BAT_DIS_OC       (1 << 7)
#define MAX77972_CHGDTLS01_CHG_DTLS_SHIFT   8
#define MAX77972_CHGDTLS01_CHG_DTLS_MASK    (0x0F << 8)
#define MAX77972_CHGDTLS01_BAT_DTLS_SHIFT   12
#define MAX77972_CHGDTLS01_BAT_DTLS_MASK    (0x07 << 12)
#define MAX77972_CHGDTLS01_TREG             (1 << 15)

// CHG_DTLS[3:0] values
#define MAX77972_CHG_DTLS_PREQUAL       0x0     // Dead/low-battery prequalification
#define MAX77972_CHG_DTLS_CC            0x1     // Fast-charge constant current
#define MAX77972_CHG_DTLS_CV            0x2     // Fast-charge constant voltage
#define MAX77972_CHG_DTLS_TOPOFF        0x3     // Top-off (not in datasheet directly, reserved)
#define MAX77972_CHG_DTLS_DONE          0x4     // Charge done (not explicitly listed, check)
#define MAX77972_CHG_DTLS_TIMER_FAULT   0x6     // Charger timer fault
#define MAX77972_CHG_DTLS_CHGEN_LOW     0x7     // CHGEN pin is low
#define MAX77972_CHG_DTLS_OFF           0x8     // Charger off (invalid input or disabled)
#define MAX77972_CHG_DTLS_REV_BOOST     0x9     // Reverse boost mode
#define MAX77972_CHG_DTLS_OVERTEMP      0xA     // Die overtemperature
#define MAX77972_CHG_DTLS_WDT_EXPIRED   0xB     // Watchdog timer expired
#define MAX77972_CHG_DTLS_OTG           0xF     // OTG mode

// ---------------------------------------------------------------------------
// NV charge voltage config: nVChgCfg1 (0x1CC / byte 0xCC on 0x37)
// RoomChargeVolt[7:0] = bits [11:4]
// VCHG[Step4][Room] = 3.4V + (RoomChargeVolt × 10mV)
// ---------------------------------------------------------------------------
#define MAX77972_VCHGCFG1_COOL_SHIFT        0
#define MAX77972_VCHGCFG1_COOL_MASK         0x000F
#define MAX77972_VCHGCFG1_ROOM_SHIFT        4
#define MAX77972_VCHGCFG1_ROOM_MASK         (0xFF << 4)
#define MAX77972_VCHGCFG1_WARM_SHIFT        12
#define MAX77972_VCHGCFG1_WARM_MASK         (0x0F << 12)

// ---------------------------------------------------------------------------
// NV charge current config: nIChgCfg1 (0x1CE / byte 0xCE on 0x37)
// RoomChargeCurr[5:0] = bits [10:5]
// ICHG[Step0][Room] = (RoomChargeCurr + 1) × 50mA
// ---------------------------------------------------------------------------
#define MAX77972_ICHGCFG1_COOL_SHIFT        0
#define MAX77972_ICHGCFG1_COOL_MASK         0x001F
#define MAX77972_ICHGCFG1_ROOM_SHIFT        5
#define MAX77972_ICHGCFG1_ROOM_MASK         (0x3F << 5)
#define MAX77972_ICHGCFG1_WARM_SHIFT        11
#define MAX77972_ICHGCFG1_WARM_MASK         (0x1F << 11)

// ---------------------------------------------------------------------------
// NLOCK unlock value (write to USR register 0xE1)
// ---------------------------------------------------------------------------
#define MAX77972_NLOCK_UNLOCK       0x0059
#define MAX77972_NLOCK_LOCK         0x0000

// ---------------------------------------------------------------------------
// CHGIN_ILIM encoding (nChgConfig3[6:0])
// 0x0-0x3: 100mA
// 0x4-0x7F: (CHGIN_ILIM + 1) × 25mA
// ---------------------------------------------------------------------------
#define MAX77972_CHGIN_ILIM_100MA   0x00
#define MAX77972_CHGIN_ILIM_500MA   0x13    // (0x13+1)*25 = 500mA
#define MAX77972_CHGIN_ILIM_1000MA  0x27    // (0x27+1)*25 = 1000mA
#define MAX77972_CHGIN_ILIM_1500MA  0x3B    // (0x3B+1)*25 = 1500mA
#define MAX77972_CHGIN_ILIM_2000MA  0x4F    // (0x4F+1)*25 = 2000mA
#define MAX77972_CHGIN_ILIM_3000MA  0x77    // (0x77+1)*25 = 3000mA

// ---------------------------------------------------------------------------
// Unit conversion macros (raw 16-bit register value → physical unit)
//
// Capacity:    0.5 mAh/LSB (unsigned)
// Percentage:  1/256 %/LSB (unsigned)
// Voltage:     78.125 µV/LSB (unsigned) = 78125 nV/LSB
// Current:     156.25 µA/LSB (signed) with 10mΩ internal Rsense
// Temperature: 1/256 °C/LSB (signed)
// Time:        5.625 s/LSB (unsigned)
// Cycles:      1/100 cycles/LSB (unsigned)
// ---------------------------------------------------------------------------

// Voltage: raw → millivolts (integer)
#define MAX77972_RAW_TO_MV(raw)         ((int32_t)(uint16_t)(raw) * 78125LL / 1000000LL)

// Current: raw → milliamps (signed integer, positive = charging)
#define MAX77972_RAW_TO_MA(raw)         ((int32_t)(int16_t)(raw) * 15625LL / 100000LL)

// Capacity: raw → milliamp-hours
#define MAX77972_RAW_TO_MAH(raw)        ((uint16_t)(raw) / 2)

// SOC: raw → integer percent (0-100)
#define MAX77972_RAW_TO_SOC(raw)        ((uint16_t)(raw) >> 8)

// Temperature: raw → integer Celsius (signed)
#define MAX77972_RAW_TO_DEGC(raw)       ((int16_t)(raw) / 256)

// Time: raw → seconds
#define MAX77972_RAW_TO_SEC(raw)        ((uint32_t)(uint16_t)(raw) * 5625UL / 1000UL)

// Cycles: raw → integer cycles (x100)
#define MAX77972_RAW_TO_CYCLES(raw)     ((uint16_t)(raw) / 100)

// RoomChargeVolt: code → millivolts
// VCHG = 3400 + code × 10 mV
#define MAX77972_ROOM_VCHG_MV(code)     (3400 + (uint32_t)(code) * 10)

// RoomChargeVolt: millivolts → code (for writing NV)
#define MAX77972_MV_TO_ROOM_VCHG(mv)    (((mv) - 3400) / 10)

// RoomChargeCurr: code → milliamps
// ICHG = (code + 1) × 50 mA
#define MAX77972_ROOM_ICHG_MA(code)     (((uint32_t)(code) + 1) * 50)

// RoomChargeCurr: milliamps → code (for writing NV)
#define MAX77972_MA_TO_ROOM_ICHG(ma)    (((ma) / 50) - 1)

// CHGIN_ILIM: code → milliamps
// For code >= 4: (code + 1) × 25 mA
#define MAX77972_CHGIN_ILIM_MA(code)    (((code) < 4) ? 100 : (((uint32_t)(code) + 1) * 25))

// CHGIN_ILIM: milliamps → code (for writing nChgConfig3)
#define MAX77972_MA_TO_CHGIN_ILIM(ma)   (((ma) <= 100) ? 0 : (((ma) / 25) - 1))
