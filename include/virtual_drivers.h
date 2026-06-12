/*
 * virtual_drivers.h — Shared virtual driver state (Phase 5)
 *
 * This header is used by BOTH the hypervisor (EL2) and guests (EL1).
 * It defines the data structures exchanged via HVC calls.
 *
 * Data flow:
 *   RTOS → HVC_FUEL_UPDATE → hypervisor stores fuel_state_t
 *   Linux → HVC_OBD_READ   → hypervisor returns fuel_state_t as OBD PIDs
 *   Android → HVC_AUDIO_STATUS → hypervisor stores/returns audio_state_t
 *
 * All structs are 64-byte aligned so they can be returned in x0-x7
 * register pairs across HVC boundaries without memory mapping.
 *
 * HVC number allocation (extends hvc_abi.h):
 *   0x0050  HVC_OBD_READ_PID     — Linux reads one OBD PID value
 *   0x0051  HVC_OBD_READ_ALL     — Linux reads full OBD snapshot
 *   0x0052  HVC_OBD_CLEAR_DTC    — Linux clears diagnostic trouble codes
 *   0x0053  HVC_OBD_GET_VIN      — Linux reads vehicle ID
 *   0x0060  HVC_FUEL_UPDATE      — RTOS writes fuel injection state
 *   0x0061  HVC_FUEL_GET_STATE   — RTOS/Linux reads current fuel state
 *   0x0062  HVC_FUEL_SET_RPM     — RTOS sets target RPM (simulation)
 *   0x0070  HVC_AUDIO_STATUS     — Android writes audio buffer status
 *   0x0071  HVC_AUDIO_PLAY       — Android signals audio playback start
 *   0x0072  HVC_AUDIO_STOP       — Android signals audio playback stop
 */

#ifndef VIRTUAL_DRIVERS_H
#define VIRTUAL_DRIVERS_H

/* ── Fuel injection state (RTOS → hypervisor → Linux OBD) ── */

#define FUEL_NUM_CYLINDERS  4

typedef struct {
    unsigned int  rpm;                        /* engine RPM 0-8000          */
    unsigned int  injection_pulse_us;         /* injection pulse width (µs) */
    unsigned int  coolant_temp_c;             /* coolant temperature °C     */
    unsigned int  throttle_pct;               /* throttle position 0-100%   */
    unsigned int  manifold_pressure_kpa;      /* MAP sensor kPa             */
    unsigned int  lambda_mv;                  /* O2 sensor mV 0-1000        */
    unsigned char cyl_status[FUEL_NUM_CYLINDERS]; /* 1=OK 0=misfire         */
    unsigned int  injection_count;            /* total injections since boot */
    unsigned int  fault_code;                 /* 0=none, P0xxx codes         */
    unsigned int  seq;                        /* sequence number for staleness*/
} fuel_state_t;

/* ── OBD-II PID definitions ── */

#define OBD_PID_RPM          0x0C   /* Engine RPM ×4 (raw) → divide by 4    */
#define OBD_PID_SPEED        0x0D   /* Vehicle speed km/h                    */
#define OBD_PID_COOLANT      0x05   /* Coolant temp °C + 40 offset           */
#define OBD_PID_THROTTLE     0x11   /* Throttle position 0-255 = 0-100%      */
#define OBD_PID_MAP          0x0B   /* Manifold absolute pressure kPa        */
#define OBD_PID_O2_S1        0x14   /* O2 sensor bank1 sensor1 voltage       */
#define OBD_PID_DTC_COUNT    0x01   /* Number of stored DTCs                 */
#define OBD_PID_VIN          0x02   /* Vehicle ID (first word)               */

typedef struct {
    unsigned int rpm;               /* actual RPM                            */
    unsigned int speed_kmh;         /* vehicle speed km/h                    */
    unsigned int coolant_temp_c;    /* coolant temperature °C                */
    unsigned int throttle_pct;      /* throttle position %                   */
    unsigned int map_kpa;           /* manifold absolute pressure kPa        */
    unsigned int o2_mv;             /* O2 sensor mV                          */
    unsigned int dtc_count;         /* number of stored DTCs                 */
    unsigned int dtc_code;          /* first DTC (P0xxx format)              */
    unsigned int seq;               /* matches fuel_state_t.seq              */
} obd_snapshot_t;

/* ── Audio driver state (Android → hypervisor) ── */

#define AUDIO_SAMPLE_RATE    44100
#define AUDIO_BUF_FRAMES     512

typedef struct {
    unsigned int  sample_rate;      /* Hz e.g. 44100                         */
    unsigned int  channels;         /* 1=mono 2=stereo                       */
    unsigned int  buf_frames;       /* frames per buffer period              */
    unsigned int  frames_played;    /* total frames played since start       */
    unsigned int  frames_dropped;   /* underrun count                        */
    unsigned int  latency_ms;       /* measured latency ms                   */
    unsigned char playing;          /* 1=active 0=stopped                    */
    unsigned char stream_id;        /* 0=music 1=notification 2=alarm        */
} audio_state_t;

/* ── HVC numbers ── */

#define HVC_OBD_READ_PID    0x0050u
#define HVC_OBD_READ_ALL    0x0051u
#define HVC_OBD_CLEAR_DTC   0x0052u
#define HVC_OBD_GET_VIN     0x0053u

#define HVC_FUEL_UPDATE     0x0060u
#define HVC_FUEL_GET_STATE  0x0061u
#define HVC_FUEL_SET_RPM    0x0062u

#define HVC_AUDIO_STATUS    0x0070u
#define HVC_AUDIO_PLAY      0x0071u
#define HVC_AUDIO_STOP      0x0072u

#endif /* VIRTUAL_DRIVERS_H */
