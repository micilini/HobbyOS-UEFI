#include "terminal.h"
#include "console.h"
#include "../utils/utils.h"


#define TERM_BG_COLOR 0xFF000022 
#define TERM_FG_COLOR 0xFFFFFFFF 
#define TERM_ACCENT   0xFF00FFFF 

void init_terminal() {
    
    console_clear(TERM_BG_COLOR);
    console_set_color(TERM_FG_COLOR, TERM_BG_COLOR);

    
    console_write("HOBBYOS V0.2 Kernel - (X64 Bare Metal Version)\n");
    console_write("(c) Portal Micilini - All Rights Reserved.\n\n");
    console_write("----------------------------------------------------------------\n\n");

    
    console_write("Available Commands:\n");
    console_write("  > help      : Show this list\n");
    console_write("  > clear     : Clear screen\n");
    console_write("  > mem       : Memory statistics\n");
    console_write("  > panic     : Test kernel panic\n\n");
    
    
    
}