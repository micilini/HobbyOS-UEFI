/* Host-derived layout for bounded read-only bootstrap captures. */
#include <stddef.h>
#include <stdio.h>
#include "../kernel/src/memory/bootstrap_tables.h"
#define FIELD(type, member) printf("\"" #member "\":%zu,", offsetof(type, member))
int main(void)
{
    printf("{\"schema\":1,\"tables\":{");
    FIELD(bootstrap_tables_t,tables); FIELD(bootstrap_tables_t,claim); FIELD(bootstrap_tables_t,main);
    FIELD(bootstrap_tables_t,count); FIELD(bootstrap_tables_t,verified); FIELD(bootstrap_tables_t,published);
    printf("\"size\":%zu},\"life\":{",sizeof(bootstrap_tables_t));
    FIELD(bootstrap_lifetime_t,generation); FIELD(bootstrap_lifetime_t,root);
    FIELD(bootstrap_lifetime_t,main_root); FIELD(bootstrap_lifetime_t,intended);
    FIELD(bootstrap_lifetime_t,state); FIELD(bootstrap_lifetime_t,count); FIELD(bootstrap_lifetime_t,cpus);
    printf("\"size\":%zu,\"cpu_stride\":%zu,\"departure\":%zu},\"departure\":{",
        sizeof(bootstrap_lifetime_t),sizeof(((bootstrap_lifetime_t *)0)->cpus[0]),
        offsetof(bootstrap_lifetime_t,cpus[0].departure)-offsetof(bootstrap_lifetime_t,cpus));
    FIELD(bootstrap_departure_t,previous_root); FIELD(bootstrap_departure_t,root);
    FIELD(bootstrap_departure_t,rsp); FIELD(bootstrap_departure_t,gdtr); FIELD(bootstrap_departure_t,idtr);
    FIELD(bootstrap_departure_t,tss); FIELD(bootstrap_departure_t,ist1); FIELD(bootstrap_departure_t,ist2);
    FIELD(bootstrap_departure_t,ist3); FIELD(bootstrap_departure_t,stack_first); FIELD(bootstrap_departure_t,stack_bytes);
    FIELD(bootstrap_departure_t,generation); FIELD(bootstrap_departure_t,apic_id); FIELD(bootstrap_departure_t,slot);
    FIELD(bootstrap_departure_t,cs); FIELD(bootstrap_departure_t,tr);
    printf("\"size\":%zu},\"snapshot\":{",sizeof(bootstrap_departure_t));
    FIELD(bootstrap_snapshot_t,generation); FIELD(bootstrap_snapshot_t,root);
    FIELD(bootstrap_snapshot_t,physical); FIELD(bootstrap_snapshot_t,bytes); FIELD(bootstrap_snapshot_t,count);
    printf("\"size\":%zu}}\n",sizeof(bootstrap_snapshot_t));
    return 0;
}
