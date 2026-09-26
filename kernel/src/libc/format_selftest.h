#ifndef FORMAT_SELFTEST_H
#define FORMAT_SELFTEST_H

#include <stdbool.h>

#if defined(HOBBYOS_FORMAT_TEST) && !defined(HOBBYOS_SELFTEST)
#error HOBBYOS_FORMAT_TEST requires HOBBYOS_SELFTEST
#endif

#if defined(HOBBYOS_FORMAT_TEST)
bool format_selftest_run(void);
bool kinit_progress_format_probe(void);
#endif

#endif
