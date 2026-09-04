#ifndef HOBBYOS_CORE_SELFTEST_H
#define HOBBYOS_CORE_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    SELFTEST_PASS = 0,
    SELFTEST_FAIL,
    SELFTEST_SKIP
} selftest_status_t;

typedef enum
{
    SELFTEST_SEVERITY_P0 = 0,
    SELFTEST_SEVERITY_P1,
    SELFTEST_SEVERITY_P2
} selftest_severity_t;

typedef struct
{
    const char *suite;
    const char *name;
    selftest_severity_t severity;
    selftest_status_t status;
    const char *detail_key;
    uint64_t expected;
    uint64_t actual;
} selftest_result_t;

typedef struct
{
    uint64_t pass;
    uint64_t fail;
    uint64_t skip;
    uint64_t p0_fail;
    uint64_t p1_fail;
    uint64_t p2_fail;
} selftest_summary_t;

typedef bool (*selftest_case_fn)(selftest_result_t *out);

typedef struct
{
    const char *suite;
    const char *name;
    selftest_severity_t severity;
    selftest_case_fn run;
} selftest_case_t;

void selftest_summary_reset(selftest_summary_t *summary);
void selftest_emit_result(const selftest_result_t *result);
void selftest_summary_add(selftest_summary_t *summary,
                          const selftest_result_t *result);
bool selftest_run_cases(const selftest_case_t *cases, uint32_t count,
                        selftest_summary_t *summary);
bool selftest_run_suite(const char *suite, selftest_summary_t *summary);
bool selftest_run_all(selftest_summary_t *summary);
bool selftest_autorun_if_enabled(void);
bool selftest_registry_validate(uint64_t *out_violations);
void selftest_emit_summary(const selftest_summary_t *summary);
void selftest_last_summary(selftest_summary_t *out);

#endif
