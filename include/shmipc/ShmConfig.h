#ifndef SHMIPC_SHMCONFIG_H
#define SHMIPC_SHMCONFIG_H

#ifndef SHMIPC_OK
#define SHMIPC_OK       0
#endif
#ifndef SHMIPC_ERR
#define SHMIPC_ERR     (-1)
#endif
#ifndef SHMIPC_TIMEOUT
#define SHMIPC_TIMEOUT (-2)
#endif

#define SHM_SERVER_MAX_UDS_PROTOCOL_FD  10
#define SHM_SERVER_MAX_PROGRESS_COUNT   2
#define SHM_SERVER_PROTOCOL_HEAD_SIZE   7

#define SHM_SERVER_DEFAULT_NAME         "shmIpc"

/* Maximum event queue slots (static array upper bound).
 * The actual runtime capacity is stored in ShmBufferEventQueue::capacity
 * and is set by init_shm_buffer_manager(). */
#define SHMIPC_MAX_EVENT_QUEUE_SIZE     512

/* ---- Preset: LOW_FREQ  (control messages, < 100/s) ---- */
#define SHMIPC_PRESET_LOW_FREQ_SHM_SIZE          ( 8u * 1024u * 1024u)  //  8 MB total
#define SHMIPC_PRESET_LOW_FREQ_EVENT_QUEUE_CAP   32u
#define SHMIPC_PRESET_LOW_FREQ_SLICE_SIZE        ( 4u * 1024u)          //  4 KB

/* ---- Preset: GENERAL  (general IPC, < 10 000/s) ---- */
#define SHMIPC_PRESET_GENERAL_SHM_SIZE           (16u * 1024u * 1024u)  // 16 MB total
#define SHMIPC_PRESET_GENERAL_EVENT_QUEUE_CAP    64u
#define SHMIPC_PRESET_GENERAL_SLICE_SIZE         (16u * 1024u)          // 16 KB

/* ---- Preset: HIGH_THROUGHPUT  (video / audio frames) ---- */
#define SHMIPC_PRESET_HIGH_THROUGHPUT_SHM_SIZE          (64u * 1024u * 1024u)  // 64 MB total
#define SHMIPC_PRESET_HIGH_THROUGHPUT_EVENT_QUEUE_CAP   256u
#define SHMIPC_PRESET_HIGH_THROUGHPUT_SLICE_SIZE        (64u * 1024u)          // 64 KB

#endif //SHMIPC_SHMCONFIG_H
