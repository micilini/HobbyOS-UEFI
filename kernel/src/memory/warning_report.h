#ifndef HOBBYOS_WARNING_REPORT_H
#define HOBBYOS_WARNING_REPORT_H
#include <stdbool.h>
#include <stddef.h>
bool warning_report_enqueue(const char *line, size_t bytes);
bool warning_report_flush(void);
bool warning_report_quiet(void);
bool warning_report_checkpoint(const char *role);
#endif
