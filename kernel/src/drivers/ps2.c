#include "ps2.h"
#include "../core/io.h"


int ps2_has_data() {
    return (inb(PS2_STATUS_PORT) & 1);
}


static void ps2_wait_write() {
    while (inb(PS2_STATUS_PORT) & 2) {
        
        io_wait();
    }
}

uint8_t ps2_read_data() {
    
    while (!ps2_has_data()) {
        io_wait();
    }
    return inb(PS2_DATA_PORT);
}

void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

void ps2_write_command(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_COMMAND_PORT, cmd);
}