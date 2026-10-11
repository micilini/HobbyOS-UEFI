#include "cmd_phystest.h"
#include "../../memory/physical_fixture.h"
#include "../diagnostic_result.h"
int cmd_phystest(int argc, char **argv)
{
    (void)argv;
    return diagnostic_result_complete("phystest",argc==1?physical_fixture_run():1);
}
