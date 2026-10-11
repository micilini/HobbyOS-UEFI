#include "cmd_wxtest.h"
#include "../../memory/protection_probe.h"
#include "../diagnostic_result.h"
int cmd_wxtest(int argc,char **argv)
{
    (void)argv;
    return diagnostic_result_complete("wxtest",argc==1 ? protection_probe_run() : 1);
}
