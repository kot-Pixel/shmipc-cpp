#ifndef SHMIPC_H
#define SHMIPC_H

#include <stdint.h>
#include <stddef.h>

/* ---- Symbol visibility ----
 * When building as a shared library every internal C++ symbol is hidden by
 * default (-fvisibility=hidden).  Only functions / variables marked with
 * SHMIPC_API are exported from the .so and visible to callers.
 * For static library builds the macro expands to nothing. */
#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef SHMIPC_BUILDING_DLL
#    define SHMIPC_API __declspec(dllexport)
#  else
#    define SHMIPC_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define SHMIPC_API __attribute__((visibility("default")))
#else
#  define SHMIPC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opaque types ---- */
typedef struct shmipc_server  shmipc_server_t;
typedef struct shmipc_client  shmipc_client_t;
typedef struct shmipc_session shmipc_session_t;

/* ---- Callback signatures ---- */
typedef void (*shmipc_on_session_cb)   (shmipc_session_t* session, void* ctx);
typedef void (*shmipc_on_data_cb)      (shmipc_session_t* session,
                                        const void* data, uint32_t len, void* ctx);
typedef void (*shmipc_on_disconnect_cb)(shmipc_session_t* session, void* ctx);

/* ---- Error codes ---- */
#define SHMIPC_OK       0
#define SHMIPC_ERR     (-1)
#define SHMIPC_TIMEOUT (-2)

/* ---- Write timeout sentinel values ---- */
/* Pass as timeout_ms to shmipc_session_write / shmipc_client_write
 * to control back-pressure behaviour. */
#define SHMIPC_TIMEOUT_NONBLOCKING  (-1)   /* drop immediately if buffer full  */
#define SHMIPC_TIMEOUT_INFINITE       0    /* block until space is available   */

/* ================================================================
 *  Channel configuration & presets
 *
 *  shm_size            : total shared memory size in bytes
 *                        (split equally into server_write and client_write regions)
 *  event_queue_capacity: ring-buffer slot count per region (≤ 512)
 *  slice_size          : data bytes per slice
 * ================================================================ */
typedef struct {
    uint32_t shm_size;
    uint32_t event_queue_capacity;
    uint32_t slice_size;
} shmipc_config_t;

/* Preset: low-frequency control messages (< 100/s)
 *   shm_size=8MB  queue=32  slice=4KB */
extern SHMIPC_API const shmipc_config_t SHMIPC_CONFIG_LOW_FREQ;

/* Preset: general-purpose IPC (< 10 000/s) — default
 *   shm_size=16MB  queue=64  slice=16KB */
extern SHMIPC_API const shmipc_config_t SHMIPC_CONFIG_GENERAL;

/* Preset: high-throughput video / audio frames
 *   shm_size=64MB  queue=256  slice=64KB */
extern SHMIPC_API const shmipc_config_t SHMIPC_CONFIG_HIGH_THROUGHPUT;

/* ================================================================
 *  Status snapshots
 *
 *  All counters are monotonically increasing since creation.
 *  send_buffer_used_pct is an instantaneous 0-100 % snapshot of
 *  how full the outbound ring-buffer is at the time of the call.
 * ================================================================ */

typedef struct {
    int      is_running;           /* 1 if the server is listening for connections */
    uint32_t connected_clients;    /* current number of active client sessions     */
} shmipc_server_status_t;

typedef struct {
    int      is_alive;             /* 1 if this session is still connected         */
    uint64_t bytes_sent;           /* bytes written by server → this client        */
    uint64_t msgs_sent;            /* messages written by server → this client     */
    uint64_t bytes_received;       /* bytes received from this client              */
    uint64_t msgs_received;        /* messages received from this client           */
    uint32_t send_buffer_used_pct; /* 0-100: server_write ring-buffer fullness     */
} shmipc_session_status_t;

typedef struct {
    int      is_connected;         /* 1 if connected to server                     */
    uint64_t bytes_sent;           /* bytes written by client → server             */
    uint64_t msgs_sent;            /* messages written by client → server          */
    uint64_t bytes_received;       /* bytes received from server                   */
    uint64_t msgs_received;        /* messages received from server                */
    uint32_t send_buffer_used_pct; /* 0-100: client_write ring-buffer fullness     */
} shmipc_client_status_t;

/* ================================================================
 *  Server API
 * ================================================================ */

SHMIPC_API shmipc_server_t* shmipc_server_create(void);
SHMIPC_API void             shmipc_server_destroy(shmipc_server_t* server);

/* Set a shared context pointer passed to every callback */
SHMIPC_API void shmipc_server_set_context(shmipc_server_t* server, void* ctx);

/* Register event callbacks */
SHMIPC_API void shmipc_server_register_on_connected   (shmipc_server_t* server, shmipc_on_session_cb    cb);
SHMIPC_API void shmipc_server_register_on_data        (shmipc_server_t* server, shmipc_on_data_cb        cb);
SHMIPC_API void shmipc_server_register_on_disconnected(shmipc_server_t* server, shmipc_on_disconnect_cb  cb);

/* Lifecycle */
SHMIPC_API int  shmipc_server_start(shmipc_server_t* server, const char* channel_name);
SHMIPC_API void shmipc_server_stop (shmipc_server_t* server);

/* Status */
SHMIPC_API void shmipc_server_get_status (shmipc_server_t*  server,  shmipc_server_status_t*  out);
SHMIPC_API void shmipc_session_get_status(shmipc_session_t* session, shmipc_session_status_t* out);

/* Data sending.
 * timeout_ms: SHMIPC_TIMEOUT_NONBLOCKING (-1) = drop immediately if full  [default]
 *             SHMIPC_TIMEOUT_INFINITE    (0)  = block until space is free
 *             N > 0                           = block up to N ms, then SHMIPC_TIMEOUT */
SHMIPC_API int shmipc_session_write(shmipc_session_t* session, const void* data, uint32_t len,
                                    int32_t timeout_ms);

/* ================================================================
 *  Client API
 * ================================================================ */

SHMIPC_API shmipc_client_t* shmipc_client_create(void);
SHMIPC_API void             shmipc_client_destroy(shmipc_client_t* client);

/* Set a shared context pointer passed to every callback */
SHMIPC_API void shmipc_client_set_context(shmipc_client_t* client, void* ctx);

/* Override channel configuration before connecting.
 * If not called, SHMIPC_CONFIG_GENERAL is used. */
SHMIPC_API void shmipc_client_set_config(shmipc_client_t* client, const shmipc_config_t* config);

/* Register event callbacks */
SHMIPC_API void shmipc_client_register_on_connected   (shmipc_client_t* client, shmipc_on_session_cb    cb);
SHMIPC_API void shmipc_client_register_on_data        (shmipc_client_t* client, shmipc_on_data_cb        cb);
SHMIPC_API void shmipc_client_register_on_disconnected(shmipc_client_t* client, shmipc_on_disconnect_cb  cb);

/* Lifecycle */
SHMIPC_API int  shmipc_client_connect   (shmipc_client_t* client, const char* channel_name);
SHMIPC_API void shmipc_client_disconnect(shmipc_client_t* client);

/* Status */
SHMIPC_API void shmipc_client_get_status(shmipc_client_t* client, shmipc_client_status_t* out);

/* Data sending — same timeout_ms semantics as shmipc_session_write */
SHMIPC_API int shmipc_client_write(shmipc_client_t* client, const void* data, uint32_t len,
                                   int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* SHMIPC_H */
