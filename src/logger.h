#ifndef LOGGER_H
#define LOGGER_H

BOOL log_module_init(LPWSTR file_nameP);
void log_module_teardown(void);
BOOL log_write(LPSTR format,  ...);

#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_TRACE 3
#define LOG_LEVEL_DETAIL 4
#endif
