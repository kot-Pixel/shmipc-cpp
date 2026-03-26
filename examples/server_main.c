#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "shmipc/shmipc.h"

typedef struct {
    int channel_id;
} MyServerCtx;

static volatile int g_running = 1;

void on_sigint(int sig) { (void)sig; g_running = 0; }

void on_client_connected(shmipc_session_t* session, void* ctx) {
    MyServerCtx* c = (MyServerCtx*)ctx;
    printf("[server][ch=%d] client connected\n", c->channel_id);

    const char* greeting = "welcome from server";
    shmipc_session_write(session, greeting, strlen(greeting), SHMIPC_TIMEOUT_NONBLOCKING);
}

void on_client_data(shmipc_session_t* session, const void* data,
                    uint32_t len, void* ctx) {
    MyServerCtx* c = (MyServerCtx*)ctx;
    printf("[server][ch=%d] recv %u bytes: %.*s\n",
           c->channel_id, len, (int)len, (const char*)data);
    shmipc_session_write(session, data, len, SHMIPC_TIMEOUT_NONBLOCKING);
}

void on_client_disconnected(shmipc_session_t* session, void* ctx) {
    MyServerCtx* c = (MyServerCtx*)ctx;
    printf("[server][ch=%d] client disconnected\n", c->channel_id);
}

int main(void) {
    signal(SIGINT, on_sigint);

    MyServerCtx ctx = { .channel_id = 1 };

    shmipc_server_t* svr = shmipc_server_create();

    shmipc_server_set_context(svr, &ctx);
    shmipc_server_register_on_connected   (svr, on_client_connected);
    shmipc_server_register_on_data        (svr, on_client_data);
    shmipc_server_register_on_disconnected(svr, on_client_disconnected);

    if (shmipc_server_start(svr, "myipc") != SHMIPC_OK) {
        printf("[server] start failed\n");
        shmipc_server_destroy(svr);
        return 1;
    }

    printf("[server] running on channel 'myipc' (Ctrl+C to stop)...\n");
    while (g_running) sleep(1);

    shmipc_server_stop(svr);
    shmipc_server_destroy(svr);
    return 0;
}
