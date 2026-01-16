#ifndef PS2_H
#define PS2_H

#include <stdint.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_CMD_ENABLE_PORT1 0xAE
#define PS2_CMD_DISABLE_PORT1 0xAD

uint8_t ps2_read_data();

void ps2_write_data(uint8_t data);
void ps2_write_command(uint8_t cmd);

int ps2_has_data();

#endif