/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rio.h"

static size_t rioCompressWrite(rio *r, const void *buf, size_t len) {
    rio_compress *rc = r->io.custom.data;
    if (!rc || !rc->target || !rc->target->write) return 0;
    return rc->target->write(rc->target, buf, len);
}

static size_t rioCompressRead(rio *r, void *buf, size_t len) {
    rio_compress *rc = r->io.custom.data;
    if (!rc || !rc->target || !rc->target->read) return 0;
    return rc->target->read(rc->target, buf, len);
}

static off_t rioCompressTell(rio *r) {
    rio_compress *rc = r->io.custom.data;
    if (!rc || !rc->target || !rc->target->tell) return 0;
    return rc->target->tell(rc->target);
}

static int rioCompressFlush(rio *r) {
    rio_compress *rc = r->io.custom.data;
    if (!rc || !rc->target || !rc->target->flush) return 0;
    return rc->target->flush(rc->target);
}

static void rioCompressUpdateChecksum(rio *r, const void *buf, size_t len) {
    rio_compress *rc = r->io.custom.data;
    if (!rc || !rc->target || !rc->target->update_cksum) return;
    rc->target->update_cksum(rc->target, buf, len);
    r->cksum = rc->target->cksum;
}

void rioInitCompress(rio_compress *rc, rio *target, const rdbFrameOpts *opts) {
    if (!rc) return;
    rc->target = target;
    if (opts) {
        rc->opts = *opts;
    } else {
        rc->opts.codec = 0;
        rc->opts.block_bytes = 0;
    }

    rc->rio_itf.read = rioCompressRead;
    rc->rio_itf.write = rioCompressWrite;
    rc->rio_itf.tell = rioCompressTell;
    rc->rio_itf.flush = rioCompressFlush;
    rc->rio_itf.update_cksum = (target && target->update_cksum) ? rioCompressUpdateChecksum : NULL;
    rc->rio_itf.cksum = target ? target->cksum : 0;
    rc->rio_itf.flags = target ? target->flags : 0;
    rc->rio_itf.processed_bytes = 0;
    rc->rio_itf.max_processing_chunk = target ? target->max_processing_chunk : 0;
    rc->rio_itf.io.custom.data = rc;
}
