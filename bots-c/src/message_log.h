#ifndef MESSAGE_LOG_H
#define MESSAGE_LOG_H

#include <stdarg.h>

#define MSG_LOG_MAX_LINES 1000
#define MSG_LOG_LINE_LEN  512

void msg_log_init(void);
void msg_log(const char *fmt, ...);
void msg_log_v(const char *fmt, va_list ap);
int  msg_log_get(const char **out, int max_count);
void msg_log_clear(void);

#endif /* MESSAGE_LOG_H */
