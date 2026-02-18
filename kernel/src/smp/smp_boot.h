#ifndef SMP_BOOT_H
#define SMP_BOOT_H

#include <stdint.h>


enum {
    CPU_STATE_DEAD = 0,
    CPU_STATE_PREPARE, 
    CPU_STATE_STARTING,
    CPU_STATE_ONLINE,  
    CPU_STATE_FAILED   
};

void smp_boot_aps();

#endif