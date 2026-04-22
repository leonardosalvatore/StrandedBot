#include "message_log.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static char         ring[MSG_LOG_MAX_LINES][MSG_LOG_LINE_LEN];
static int          ring_head  = 0;
static int          ring_count = 0;
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;

void msg_log_init(void) {
    pthread_mutex_lock(&log_mtx);
    ring_head  = 0;
    ring_count = 0;
    pthread_mutex_unlock(&log_mtx);
}

void msg_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    msg_log_v(fmt, ap);
    va_end(ap);
}

void msg_log_v(const char *fmt, va_list ap) {
    char buf[MSG_LOG_LINE_LEN];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    /* echo to stdout */
    fprintf(stdout, "%s\n", buf);
    fflush(stdout);

    pthread_mutex_lock(&log_mtx);
    int idx = (ring_head + ring_count) % MSG_LOG_MAX_LINES;
    if (ring_count == MSG_LOG_MAX_LINES) {
        ring_head = (ring_head + 1) % MSG_LOG_MAX_LINES;
    } else {
        ring_count++;
    }
    strncpy(ring[idx], buf, MSG_LOG_LINE_LEN - 1);
    ring[idx][MSG_LOG_LINE_LEN - 1] = '\0';
    pthread_mutex_unlock(&log_mtx);
}

int msg_log_get(const char **out, int max_count) {
    pthread_mutex_lock(&log_mtx);
    int n = ring_count < max_count ? ring_count : max_count;
    int start = (ring_head + ring_count - n) % MSG_LOG_MAX_LINES;
    for (int i = 0; i < n; i++) {
        out[i] = ring[(start + i) % MSG_LOG_MAX_LINES];
    }
    pthread_mutex_unlock(&log_mtx);
    return n;
}

void msg_log_clear(void) {
    pthread_mutex_lock(&log_mtx);
    ring_head  = 0;
    ring_count = 0;
    pthread_mutex_unlock(&log_mtx);
}
