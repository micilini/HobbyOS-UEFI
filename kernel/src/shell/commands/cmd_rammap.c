#include "cmd_rammap.h"
#include "../../memory/ram_map_fixture.h"
#include "../../libc/string.h"
#include "../diagnostic_result.h"
int cmd_rammap(int argc, char **argv)
{
    bool high=argc==2 && !strcmp(argv[1],"high");
    return diagnostic_result_complete("rammap",(argc==1 || high)?ram_map_fixture_run(high):1);
}
