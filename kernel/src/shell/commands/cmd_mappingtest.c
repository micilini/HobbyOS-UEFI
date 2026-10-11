#include "cmd_mappingtest.h"
#include "../../memory/mapping_probe.h"
#include "../../libc/string.h"
#include "../diagnostic_result.h"
int cmd_mappingtest(int argc, char **argv)
{
    if(argc==1 || (argc==2 && !strcmp(argv[1],"run")))
        return diagnostic_result_complete("mappingtest",mapping_fixture_run());
    if(argc==2 && !strcmp(argv[1],"framebuffer-copy"))
        return diagnostic_result_complete("mappingtest framebuffer-copy",framebuffer_copy_benchmark());
    return diagnostic_result_complete("mappingtest",1);
}
