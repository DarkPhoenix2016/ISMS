#ifndef NODE_MANAGER_H
#define NODE_MANAGER_H

#include <Arduino.h>

void initNodeManager();
void updateNodeManager();

// Quick reachability check + add to discovered list.
// Returns true if the node answered HTTP within 1.5 s.
// The normal poll cycle will fetch full readings within 5 s.
bool addNodeByIP(const String& ip);

// Returns true once NTP has synced (time() > 1 Jan 2001).
// Used by db_sync to guard against inserting 1970 timestamps. (I5/B7)
bool ntpSynced();

#endif
