#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "shmipc/shmipc.h"

typedef struct {
    const char* name;
} MyClientCtx;

static volatile int g_running = 1;

void on_sigint(int sig) { (void)sig; g_running = 0; }

void on_server_connected(shmipc_session_t* session, void* ctx) {
    MyClientCtx* c = (MyClientCtx*)ctx;
    printf("[client][%s] connected to server\n", c->name);
}

void on_server_data(shmipc_session_t* session, const void* data,
                    uint32_t len, void* ctx) {
    MyClientCtx* c = (MyClientCtx*)ctx;
    printf("[client][%s] recv %u bytes: %.*s\n",
           c->name, len, (int)len, (const char*)data);
}

void on_server_disconnected(shmipc_session_t* session, void* ctx) {
    MyClientCtx* c = (MyClientCtx*)ctx;
    printf("[client][%s] server disconnected\n", c->name);
    g_running = 0;
}

int main(void) {
    signal(SIGINT, on_sigint);

    MyClientCtx ctx = { .name = "client-1" };

    shmipc_client_t* cli = shmipc_client_create();

    shmipc_client_set_context(cli, &ctx);

    /* Use HIGH_THROUGHPUT preset — remove this line to use GENERAL (default) */
    shmipc_client_set_config(cli, &SHMIPC_CONFIG_HIGH_THROUGHPUT);

    shmipc_client_register_on_connected   (cli, on_server_connected);
    shmipc_client_register_on_data        (cli, on_server_data);
    shmipc_client_register_on_disconnected(cli, on_server_disconnected);

    if (shmipc_client_connect(cli, "myipc") != SHMIPC_OK) {
        printf("[client] connect failed\n");
        shmipc_client_destroy(cli);
        return 1;
    }

    const char* msg = "hello from client";
    /* Block up to 200 ms if the send buffer is temporarily full */
    shmipc_client_write(cli, msg, strlen(msg), 200);

    printf("[client] running (Ctrl+C to stop)...\n");
    while (g_running) sleep(1);

    shmipc_client_disconnect(cli);
    shmipc_client_destroy(cli);
    return 0;
}
