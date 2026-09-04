#ifndef CMD_REAPTEST_H
#define CMD_REAPTEST_H
#include "../../core/task.h"
int cmd_reaptest(int argc,char **argv);
void reaptest_zombie_mem_observe_ps(task_id_t id,uint64_t bytes);
void reaptest_zombie_mem_observe_taskman(task_id_t id,uint64_t bytes);
#endif
