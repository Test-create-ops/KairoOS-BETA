#ifndef LOG_H
#define LOG_H

void log_info(const char *msg);
void log_warn(const char *msg);
void log_error(const char *msg);
void log_hex(unsigned long value);

#endif
