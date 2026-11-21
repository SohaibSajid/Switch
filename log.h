#ifndef LOG_H
#define LOG_H

/* Messages are put on a queue and one logger thread writes them to the
 * file, so the threads that forward frames never wait for the disk.
 * If the logger is not running, messages go straight to stderr. */

int log_start(const char *path, int echo);
void log_stop(void);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
