#ifndef CMD_KILLTEST_H
#define CMD_KILLTEST_H
#ifdef HOBBYOS_SELFTEST
int cmd_killtest(int argc,char **argv);
int killtest_cli_telemetry_enabled(void);
int killtest_ui_telemetry_enabled(void);
#else
static inline int killtest_cli_telemetry_enabled(void) { return 0; }
static inline int killtest_ui_telemetry_enabled(void) { return 0; }
#endif
#endif
