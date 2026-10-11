#include "cmd_stacktest.h"
#include "../../memory/guard_probe.h"
#include "../../libc/string.h"
#include "../diagnostic_result.h"
int cmd_stacktest(int argc,char **argv)
{
    if (argc==1 || (argc==2 && !strcmp(argv[1],"inventory")))
        return diagnostic_result_complete("stacktest",stack_test_run(false));
    if (argc==2 && !strcmp(argv[1],"churn"))
        return diagnostic_result_complete("stacktest churn",stack_test_run(true));
    if (argc==2 && !strcmp(argv[1],"overflow"))
        return diagnostic_result_complete("stacktest overflow",stack_test_overflow());
    if (argc==2 && !strcmp(argv[1],"timeout"))
        return diagnostic_result_complete("stacktest timeout",stack_test_timeout());
    return diagnostic_result_complete("stacktest",1);
}
