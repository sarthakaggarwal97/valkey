#ifndef MEMORY_PREFETCH_H
#define MEMORY_PREFETCH_H

#include "adlist.h"

struct client;

void prefetchCommandsBatchInit(void);
void processClientsCommandsBatch(list *handled_clients);
int addCommandToBatchAndProcessIfFull(struct client *c, list *handled_clients);
void removeClientFromPendingCommandsBatch(struct client *c);
int onMaxBatchSizeChange(const char **err);

#endif /* MEMORY_PREFETCH_H */
