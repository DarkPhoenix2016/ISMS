#ifndef DB_SYNC_H
#define DB_SYNC_H

#include <stdint.h>

extern bool     g_syncLastOk;
extern uint32_t g_syncLastTime;   // epoch seconds of last successful sync

void initDBSync();
void updateDBSync();

#endif
