#ifndef MSU_STATS_H_
#define MSU_STATS_H_

#include "timeseries.h"
#include "stats.h"
#include "dfg.h"
#include "unused_def.h"

struct stat_item {
    unsigned int id;
    struct timed_rrdb stats;
};

#define MAX_STAT_ID 4192


struct timed_rrdb *get_stat(enum stat_id id, unsigned int item_id);

int unregister_msu_stat(unsigned int msu_id);
int register_msu_stat(unsigned int msu_id, int thread_id, int runtime_id);
int init_statistics();

void show_stats(struct dfg_msu *msu);

#endif
