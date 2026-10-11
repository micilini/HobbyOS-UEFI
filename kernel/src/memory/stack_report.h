#ifndef HOBBYOS_STACK_REPORT_H
#define HOBBYOS_STACK_REPORT_H
#include <stdbool.h>
#include <stddef.h>
bool stack_report_enqueue(const char *line, size_t bytes);
bool stack_report_flush(void);
bool stack_report_quiet(void);
bool stack_report_checkpoint(const char *role);
#endif
