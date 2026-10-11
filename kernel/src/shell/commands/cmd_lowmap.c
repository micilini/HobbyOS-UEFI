#include "cmd_lowmap.h"
#include "../../memory/null_probe.h"
#include "../../libc/string.h"
#include "../diagnostic_result.h"
int cmd_lowmap(int argc, char **argv)
{
    if (argc==1 || (argc==2 && !strcmp(argv[1],"run")))
        return diagnostic_result_complete("lowmap",null_probe_run());
    if (argc==2 && !strcmp(argv[1],"crash"))
        return diagnostic_result_complete("lowmap crash",null_probe_destructive());
    return diagnostic_result_complete("lowmap",1);
}
