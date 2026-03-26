#ifndef SHMIPC_SHMLOGGER_H
#define SHMIPC_SHMLOGGER_H

#include <cstdio>
#include <cstring>
#include <cerrno>

#define SHMIPC_LOG_TAG "shmipc"

#define LOGV(fmt, ...) fprintf(stdout, "[VERBOSE][%s] " fmt "\n", SHMIPC_LOG_TAG, ##__VA_ARGS__)
#define LOGD(fmt, ...) fprintf(stdout, "[DEBUG][%s] " fmt "\n", SHMIPC_LOG_TAG, ##__VA_ARGS__)
#define LOGI(fmt, ...) fprintf(stdout, "[INFO][%s] " fmt "\n", SHMIPC_LOG_TAG, ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "[WARN][%s] " fmt "\n", SHMIPC_LOG_TAG, ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, "[ERROR][%s] " fmt "\n", SHMIPC_LOG_TAG, ##__VA_ARGS__)
#define LOGF(fmt, ...) fprintf(stderr, "[FATAL][%s] " fmt "\n", SHMIPC_LOG_TAG, ##__VA_ARGS__)

#endif //SHMIPC_SHMLOGGER_H
