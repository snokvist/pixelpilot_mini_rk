#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>

const char *log_timestamp(void);
int log_is_verbose(void);
void log_set_verbose(int enabled);

#define LOGI(fmt, ...) fprintf(stderr, "[%s] [I] " fmt "\n", log_timestamp(), ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "[%s] [W] " fmt "\n", log_timestamp(), ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, "[%s] [E] " fmt "\n", log_timestamp(), ##__VA_ARGS__)
#define LOGV(fmt, ...) do { if (log_is_verbose()) fprintf(stderr, "[%s] [D] " fmt "\n", log_timestamp(), ##__VA_ARGS__); } while (0)

#endif // LOGGING_H
