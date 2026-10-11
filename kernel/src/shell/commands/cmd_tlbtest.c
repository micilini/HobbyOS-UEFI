#include "cmd_tlbtest.h"
#include "../../memory/tlb_fixture.h"
#include "../../libc/string.h"
#include "../diagnostic_result.h"

int cmd_tlbtest(int argc, char **argv)
{
    if(argc==1 || (argc==2 && !strcmp(argv[1],"run")))
        return diagnostic_result_complete("tlbtest",tlb_fixture_run());
    if(argc==2 && !strcmp(argv[1],"context-failure"))
        return diagnostic_result_complete("tlbtest context-failure",tlb_fixture_context_failure());
    if(argc==2 && !strcmp(argv[1],"delayed-response"))
        return diagnostic_result_complete("tlbtest delayed-response",tlb_fixture_delayed_response());
    return 2;
}
