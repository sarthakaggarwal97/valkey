/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_engine_stats.h -- counters of the JSON storage engine that are not
 * part of its API: for tests and diagnostics. */

#ifndef JSON_ENGINE_STATS_H
#define JSON_ENGINE_STATS_H

#include <stdint.h>

/* How a change to a document was made (design 8.3). */
enum {
    JSON_WRITE_IN_PLACE = 0, /* Bytes of a packed node overwritten, its size unchanged. */
    JSON_WRITE_SPLICE,       /* A byte range of a packed node replaced, sizes patched. */
    JSON_WRITE_REEMIT,       /* A packed node walked onto a tape and emitted again. */
    JSON_WRITE_SLOT,         /* A spine container or the document word written. */
    JSON_WRITE_KINDS
};

/* Number of changes made by each primitive since jsonEngineInit. One call of
 * a jsonSet / jsonDelete / ... function counts once, except jsonArrayTrim,
 * which counts once for each end it cuts. */
void jsonEngineGetWriteStats(uint64_t counts[JSON_WRITE_KINDS]);

#endif /* JSON_ENGINE_STATS_H */
