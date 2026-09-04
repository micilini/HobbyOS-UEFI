#ifndef INPUT_EVENT_H
#define INPUT_EVENT_H

#include <stdint.h>

#define KEY_SPECIAL_LEFT      0xF1
#define KEY_SPECIAL_RIGHT     0xF2
#define KEY_SPECIAL_UP        0xF3
#define KEY_SPECIAL_DOWN      0xF4
#define KEY_SPECIAL_PAGE_UP   0xF5
#define KEY_SPECIAL_PAGE_DOWN 0xF6
#define KEY_SPECIAL_HOME      0xF7
#define KEY_SPECIAL_END       0xF8



typedef enum
{
    INPUT_EVENT_CHAR    = 0,
    INPUT_EVENT_SPECIAL = 1
} input_event_type_t;

typedef struct
{
    uint8_t type;
    uint8_t value;
    uint8_t _pad[2];
} input_event_t;



static inline input_event_t input_event_char(char c)
{
    input_event_t ev;
    ev.type = INPUT_EVENT_CHAR;
    ev.value = (uint8_t)c;
    ev._pad[0] = 0;
    ev._pad[1] = 0;
    return ev;
}

static inline input_event_t input_event_special(uint8_t key)
{
    input_event_t ev;
    ev.type = INPUT_EVENT_SPECIAL;
    ev.value = key;
    ev._pad[0] = 0;
    ev._pad[1] = 0;
    return ev;
}

#endif
