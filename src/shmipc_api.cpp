#include "shmipc/shmipc.h"
#include "shmipc/ShmBufferManager.h"
#include "shmipc/ShmServer.h"
#include "shmipc/ShmClient.h"

/* ---- Preset definitions ---- */
const shmipc_config_t SHMIPC_CONFIG_LOW_FREQ = {
    SHMIPC_PRESET_LOW_FREQ_SHM_SIZE,
    SHMIPC_PRESET_LOW_FREQ_EVENT_QUEUE_CAP,
    SHMIPC_PRESET_LOW_FREQ_SLICE_SIZE
};

const shmipc_config_t SHMIPC_CONFIG_GENERAL = {
    SHMIPC_PRESET_GENERAL_SHM_SIZE,
    SHMIPC_PRESET_GENERAL_EVENT_QUEUE_CAP,
    SHMIPC_PRESET_GENERAL_SLICE_SIZE
};

const shmipc_config_t SHMIPC_CONFIG_HIGH_THROUGHPUT = {
    SHMIPC_PRESET_HIGH_THROUGHPUT_SHM_SIZE,
    SHMIPC_PRESET_HIGH_THROUGHPUT_EVENT_QUEUE_CAP,
    SHMIPC_PRESET_HIGH_THROUGHPUT_SLICE_SIZE
};

/* ---- Internal types ---- */

struct shmipc_session {
    enum { SERVER_SIDE, CLIENT_SIDE } type;
    union {
        ShmServerSession* server_session;
        ShmClientSession* client_session;
    };
};

struct shmipc_server {
    ShmServer impl;

    void*                   ctx                = nullptr;
    shmipc_on_session_cb    on_connected_cb    = nullptr;
    shmipc_on_data_cb       on_data_cb         = nullptr;
    shmipc_on_disconnect_cb on_disconnected_cb = nullptr;
};

struct shmipc_client {
    ShmClient      impl;
    shmipc_session session;

    void*                        ctx                = nullptr;
    shmipc_on_session_cb         on_connected_cb    = nullptr;
    shmipc_on_data_cb            on_data_cb         = nullptr;
    shmipc_cli_on_data_zc_cb     on_data_zc_cb      = nullptr;
    shmipc_on_disconnect_cb      on_disconnected_cb = nullptr;
};

static shmipc_session* get_or_create_server_session_handle(ShmServerSession* ss) {
    if (!ss->apiHandle) {
        auto* h           = new shmipc_session;
        h->type           = shmipc_session::SERVER_SIDE;
        h->server_session = ss;
        ss->apiHandle     = h;
    }
    return static_cast<shmipc_session*>(ss->apiHandle);
}

/* ================================================================
 *  Zero-copy receive-buffer accessors
 * ================================================================ */
extern "C" {

const void* shmipc_buf_data(const shmipc_buf_t* buf) {
    return buf ? buf->data : nullptr;
}

uint32_t shmipc_buf_len(const shmipc_buf_t* buf) {
    return buf ? buf->len : 0;
}

void shmipc_buf_release(shmipc_buf_t* buf) {
    shmipc_buf_free(buf);  /* delegates to the inline helper in ShmBufferManager.h */
}

} // extern "C"

/* ================================================================
 *  Server API
 * ================================================================ */
extern "C" {

shmipc_server_t* shmipc_server_create(void) {
    return new shmipc_server;
}

void shmipc_server_destroy(shmipc_server_t* s) {
    if (!s) return;
    s->impl.stop();
    for (auto* ss : s->impl.getAllSessions()) {
        if (ss->apiHandle) {
            delete static_cast<shmipc_session*>(ss->apiHandle);
            ss->apiHandle = nullptr;
        }
    }
    delete s;
}

void shmipc_server_set_context(shmipc_server_t* s, void* ctx) {
    if (s) s->ctx = ctx;
}

void shmipc_server_register_on_connected(shmipc_server_t* s, shmipc_on_session_cb cb) {
    if (!s) return;
    s->on_connected_cb = cb;
    s->impl.setOnConnected([s](ShmServerSession* ss) {
        if (s->on_connected_cb)
            s->on_connected_cb(get_or_create_server_session_handle(ss), s->ctx);
    });
}

void shmipc_server_register_on_data(shmipc_server_t* s, shmipc_on_data_cb cb) {
    if (!s) return;
    s->on_data_cb = cb;
    s->impl.setOnData([s](ShmServerSession* ss, const void* data, uint32_t len) {
        if (s->on_data_cb)
            s->on_data_cb(get_or_create_server_session_handle(ss), data, len, s->ctx);
    });
}

void shmipc_server_register_on_data_zc(shmipc_server_t* s, shmipc_on_data_zc_cb cb) {
    if (!s) return;
    s->impl.setOnDataZc([s, cb](ShmServerSession* ss, shmipc_buf_t* buf) {
        if (cb) cb(get_or_create_server_session_handle(ss), buf, s->ctx);
    });
}

void shmipc_server_register_on_disconnected(shmipc_server_t* s, shmipc_on_disconnect_cb cb) {
    if (!s) return;
    s->on_disconnected_cb = cb;
    s->impl.setOnDisconnected([s](ShmServerSession* ss) {
        if (s->on_disconnected_cb) {
            auto* h = get_or_create_server_session_handle(ss);
            s->on_disconnected_cb(h, s->ctx);
            delete h;
            ss->apiHandle = nullptr;
        }
    });
}

int shmipc_server_start(shmipc_server_t* s, const char* channel_name) {
    if (!s) return SHMIPC_ERR;
    return s->impl.start(channel_name);
}

void shmipc_server_stop(shmipc_server_t* s) {
    if (s) s->impl.stop();
}

int shmipc_session_write(shmipc_session_t* session, const void* data, uint32_t len,
                         int32_t timeout_ms) {
    if (!session || !data || len == 0) return SHMIPC_ERR;
    if (session->type == shmipc_session::SERVER_SIDE) {
        return session->server_session->writData(
                static_cast<const uint8_t*>(data), len, timeout_ms);
    } else {
        return session->client_session->writData(
                static_cast<const uint8_t*>(data), len, timeout_ms);
    }
}


/* ================================================================
 *  Client API
 * ================================================================ */

shmipc_client_t* shmipc_client_create(void) {
    auto* c = new shmipc_client;
    c->session.type           = shmipc_session::CLIENT_SIDE;
    c->session.client_session = c->impl.session();
    c->impl.session()->apiHandle = &c->session;
    return c;
}

void shmipc_client_destroy(shmipc_client_t* c) {
    if (!c) return;
    c->impl.disconnect();
    delete c;
}

void shmipc_client_set_context(shmipc_client_t* c, void* ctx) {
    if (c) c->ctx = ctx;
}

void shmipc_client_set_config(shmipc_client_t* c, const shmipc_config_t* cfg) {
    if (!c || !cfg) return;
    c->impl.session()->setConfig(cfg->shm_size,
                                  cfg->event_queue_capacity,
                                  cfg->slice_size);
}

void shmipc_client_register_on_connected(shmipc_client_t* c, shmipc_on_session_cb cb) {
    if (!c) return;
    c->on_connected_cb = cb;
    c->impl.setOnConnected([c]() {
        if (c->on_connected_cb) c->on_connected_cb(&c->session, c->ctx);
    });
}

void shmipc_client_register_on_data(shmipc_client_t* c, shmipc_on_data_cb cb) {
    if (!c) return;
    c->on_data_cb = cb;
    c->impl.setOnData([c](const void* data, uint32_t len) {
        if (c->on_data_cb) c->on_data_cb(&c->session, data, len, c->ctx);
    });
}

void shmipc_client_register_on_data_zc(shmipc_client_t* c, shmipc_cli_on_data_zc_cb cb) {
    if (!c) return;
    c->on_data_zc_cb = cb;
    c->impl.setOnDataZc([c, cb](shmipc_buf_t* buf) {
        if (cb) cb(c, buf, c->ctx);
    });
}

void shmipc_client_register_on_disconnected(shmipc_client_t* c, shmipc_on_disconnect_cb cb) {
    if (!c) return;
    c->on_disconnected_cb = cb;
    c->impl.setOnDisconnected([c]() {
        if (c->on_disconnected_cb) c->on_disconnected_cb(&c->session, c->ctx);
    });
}

int shmipc_client_connect(shmipc_client_t* c, const char* channel_name) {
    if (!c) return SHMIPC_ERR;
    return c->impl.connect(channel_name);
}

void shmipc_client_disconnect(shmipc_client_t* c) {
    if (c) c->impl.disconnect();
}

int shmipc_client_write(shmipc_client_t* c, const void* data, uint32_t len,
                        int32_t timeout_ms) {
    if (!c || !data || len == 0) return SHMIPC_ERR;
    return c->impl.writData(static_cast<const uint8_t*>(data), len, timeout_ms);
}

void shmipc_client_get_status(shmipc_client_t* c, shmipc_client_status_t* out) {
    if (!c || !out) return;
    c->impl.getStatus(out);
}

void shmipc_client_get_latency(shmipc_client_t* c, shmipc_latency_stats_t* out) {
    if (!c || !out) return;
    c->impl.session()->getLatency(out);
}

void shmipc_client_reset_latency(shmipc_client_t* c) {
    if (!c) return;
    c->impl.session()->resetLatency();
}

/* ---- Server status ---- */

void shmipc_server_get_status(shmipc_server_t* s, shmipc_server_status_t* out) {
    if (!s || !out) return;
    out->is_running        = s->impl.isRunning() ? 1 : 0;
    out->connected_clients = s->impl.getConnectedCount();
}

void shmipc_session_get_status(shmipc_session_t* session, shmipc_session_status_t* out) {
    if (!session || !out) return;
    if (session->type == shmipc_session::SERVER_SIDE)
        session->server_session->getStatus(out);
}

void shmipc_session_get_latency(shmipc_session_t* session, shmipc_latency_stats_t* out) {
    if (!session || !out) return;
    if (session->type == shmipc_session::SERVER_SIDE)
        session->server_session->getLatency(out);
}

void shmipc_session_reset_latency(shmipc_session_t* session) {
    if (!session) return;
    if (session->type == shmipc_session::SERVER_SIDE)
        session->server_session->resetLatency();
}

/* ================================================================
 *  Write-side zero-copy (Feature 6)
 * ================================================================ */

void* shmipc_wbuf_data(shmipc_wbuf_t* buf) {
    return buf ? buf->data : nullptr;
}
uint32_t shmipc_wbuf_capacity(shmipc_wbuf_t* buf) {
    return buf ? buf->capacity : 0u;
}

shmipc_wbuf_t* shmipc_session_alloc_buf(shmipc_session_t* s, uint32_t len) {
    if (!s || s->type != shmipc_session::SERVER_SIDE) return nullptr;
    return s->server_session->allocWriteBuf(len);
}
int shmipc_session_send_buf(shmipc_session_t* s, shmipc_wbuf_t* buf, uint32_t len) {
    if (!s || s->type != shmipc_session::SERVER_SIDE) { if (buf) s->server_session->discardWriteBuf(buf); return SHMIPC_ERR; }
    return s->server_session->sendWriteBuf(buf, len);
}
void shmipc_session_discard_buf(shmipc_session_t* s, shmipc_wbuf_t* buf) {
    if (s && s->type == shmipc_session::SERVER_SIDE)
        s->server_session->discardWriteBuf(buf);
}

shmipc_wbuf_t* shmipc_client_alloc_buf(shmipc_client_t* c, uint32_t len) {
    if (!c || !c->impl.session()) return nullptr;
    return c->impl.session()->allocWriteBuf(len);
}
int shmipc_client_send_buf(shmipc_client_t* c, shmipc_wbuf_t* buf, uint32_t len) {
    if (!c || !c->impl.session()) { if (c && c->impl.session()) c->impl.session()->discardWriteBuf(buf); return SHMIPC_ERR; }
    return c->impl.session()->sendWriteBuf(buf, len);
}
void shmipc_client_discard_buf(shmipc_client_t* c, shmipc_wbuf_t* buf) {
    if (c && c->impl.session()) c->impl.session()->discardWriteBuf(buf);
}

/* ================================================================
 *  Async dispatch (Feature 7)
 * ================================================================ */

void shmipc_server_set_async_dispatch(shmipc_server_t* srv, uint32_t depth) {
    if (srv) srv->impl.setAsyncDispatchDepth(depth);
}

void shmipc_client_set_async_dispatch(shmipc_client_t* cli, uint32_t depth) {
    if (cli && cli->impl.session()) cli->impl.session()->setAsyncDispatchDepth(depth);
}

} // extern "C"
