/* rio.c is a simple stream-oriented I/O abstraction that provides an interface
 * to write code that can consume/produce data using different concrete input
 * and output devices. For instance the same rdb.c code using the rio
 * abstraction can be used to read and write the RDB format using in-memory
 * buffers or files.
 *
 * A rio object provides the following methods:
 *  read: read from stream.
 *  write: write to stream.
 *  tell: get the current offset.
 *
 * It is also possible to set a 'checksum' method that is used by rio.c in order
 * to compute a checksum of the data written or read, or to query the rio object
 * for the current checksum.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "fmacros.h"
#include "fpconv_dtoa.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rio.h"
#include "util.h"
#include "crc64.h"
#include "config.h"
#include "server.h"
#include "connhelpers.h"
#include "compression_stream.h"

/* ------------------------- Buffer I/O implementation ----------------------- */

/* Returns 1 or 0 for success/failure. */
static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
    r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr, (char *)buf, len);
    r->io.buffer.pos += len;
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioBufferRead(rio *r, void *buf, size_t len) {
    if (sdslen(r->io.buffer.ptr) - r->io.buffer.pos < len) return 0; /* not enough buffer to return len bytes. */
    memcpy(buf, r->io.buffer.ptr + r->io.buffer.pos, len);
    r->io.buffer.pos += len;
    return 1;
}

/* Returns read/write position in buffer. */
static off_t rioBufferTell(rio *r) {
    return r->io.buffer.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioBufferFlush(rio *r) {
    UNUSED(r);
    return 1; /* Nothing to do, our write just appends to the buffer. */
}

static const rio rioBufferIO = {
    .read = rioBufferRead,
    .write = rioBufferWrite,
    .tell = rioBufferTell,
    .flush = rioBufferFlush,
    .read_some = NULL,
    .update_cksum = NULL,
    .cksum = 0,
    .flags = 0,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {{NULL, 0}},
};

void rioInitWithBuffer(rio *r, sds s) {
    *r = rioBufferIO;
    r->io.buffer.ptr = s;
    r->io.buffer.pos = 0;
}

/* --------------------- Stdio file pointer implementation ------------------- */

/* Returns 1 or 0 for success/failure. */
static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
    if (!r->io.file.autosync) return fwrite(buf, len, 1, r->io.file.fp);

    size_t nwritten = 0;
    /* Incrementally write data to the file, avoid a single write larger than
     * the autosync threshold (so that the kernel's buffer cache never has too
     * many dirty pages at once). */
    while (len != nwritten) {
        serverAssert(r->io.file.autosync > r->io.file.buffered);
        size_t nalign = (size_t)(r->io.file.autosync - r->io.file.buffered);
        size_t towrite = nalign > len - nwritten ? len - nwritten : nalign;

        if (fwrite((char *)buf + nwritten, towrite, 1, r->io.file.fp) == 0) return 0;
        nwritten += towrite;
        r->io.file.buffered += towrite;

        if (r->io.file.buffered >= r->io.file.autosync) {
            fflush(r->io.file.fp);

            size_t processed = r->processed_bytes + nwritten;
            serverAssert(processed % r->io.file.autosync == 0);
            serverAssert(r->io.file.buffered == r->io.file.autosync);

#if defined(HAVE_SYNC_FILE_RANGE) && HAVE_SYNC_FILE_RANGE
            /* Start writeout asynchronously. */
            if (sync_file_range(fileno(r->io.file.fp), processed - r->io.file.autosync, r->io.file.autosync,
                                SYNC_FILE_RANGE_WRITE) == -1)
                return 0;

            if (processed >= (size_t)r->io.file.autosync * 2) {
                /* To keep the promise to 'autosync', we should make sure last
                 * asynchronous writeout persists into disk. This call may block
                 * if last writeout is not finished since disk is slow. */
                if (sync_file_range(fileno(r->io.file.fp), processed - r->io.file.autosync * 2, r->io.file.autosync,
                                    SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) ==
                    -1)
                    return 0;
            }
#else
            if (valkey_fsync(fileno(r->io.file.fp)) == -1) return 0;
#endif
            if (r->io.file.reclaim_cache) {
                /* In Linux sync_file_range just issue a writeback request to
                 * OS, and when posix_fadvise is called, the dirty page may
                 * still be in flushing, which means it would be ignored by
                 * posix_fadvise.
                 *
                 * So we posix_fadvise the whole file, and the writeback-ed
                 * pages will have other chances to be reclaimed. */
                reclaimFilePageCache(fileno(r->io.file.fp), 0, 0);
            }
            r->io.file.buffered = 0;
        }
    }
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioFileRead(rio *r, void *buf, size_t len) {
    return fread(buf, 1, len, r->io.file.fp) == len;
}

/* Partial-read variant: returns bytes read (may be less than len), 0 on EOF, -1 on error. */
static ssize_t rioFileReadSome(rio *r, void *buf, size_t len) {
    size_t got = fread(buf, 1, len, r->io.file.fp);
    if (got == 0 && ferror(r->io.file.fp)) return -1;
    return (ssize_t)got;
}

/* Returns read/write position in file. */
static off_t rioFileTell(rio *r) {
    return ftello(r->io.file.fp);
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFileFlush(rio *r) {
    return (fflush(r->io.file.fp) == 0) ? 1 : 0;
}

static const rio rioFileIO = {
    .read = rioFileRead,
    .write = rioFileWrite,
    .tell = rioFileTell,
    .flush = rioFileFlush,
    .read_some = rioFileReadSome,
    .update_cksum = NULL,
    .cksum = 0,
    .flags = 0,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {{NULL, 0}},
};

void rioInitWithFile(rio *r, FILE *fp) {
    *r = rioFileIO;
    r->io.file.fp = fp;
    r->io.file.buffered = 0;
    r->io.file.autosync = 0;
    r->io.file.reclaim_cache = 0;
}

/* ------------------- Connection implementation -------------------
 * We use this RIO implementation when reading an RDB file directly from
 * the connection to the memory via rdbLoadRio(), thus this implementation
 * only implements reading from a connection that is, normally,
 * just a socket. */

static size_t rioConnWrite(rio *r, const void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not yet support writing. */
}

/* Fill the connection read buffer until at least min_available bytes are available.
 * Returns 1 on success, 0 on EOF, -1 on error. */
static int rioConnEnsureBuffered(rio *r, size_t min_available) {
    size_t avail = sdslen(r->io.conn.buf) - r->io.conn.pos;

    /* If the buffer is too small for the entire request: realloc. */
    if (sdslen(r->io.conn.buf) + sdsavail(r->io.conn.buf) < min_available)
        r->io.conn.buf = sdsMakeRoomFor(r->io.conn.buf, min_available - sdslen(r->io.conn.buf));

    /* If the remaining unused buffer is not large enough: memmove so that we
     * can read the rest. */
    if (min_available > avail && sdsavail(r->io.conn.buf) < min_available - avail) {
        sdsrange(r->io.conn.buf, r->io.conn.pos, -1);
        r->io.conn.pos = 0;
    }

    while (avail < min_available) {
        size_t needs = min_available - avail;
        /* Read either what's missing, or PROTO_IOBUF_LEN, the bigger of
         * the two. */
        size_t toread = needs < PROTO_IOBUF_LEN ? PROTO_IOBUF_LEN : needs;
        if (toread > sdsavail(r->io.conn.buf)) toread = sdsavail(r->io.conn.buf);
        if (r->io.conn.read_limit != 0) {
            size_t remaining =
                r->io.conn.read_so_far >= r->io.conn.read_limit ? 0 : r->io.conn.read_limit - r->io.conn.read_so_far;
            if (avail >= remaining) {
                toread = 0;
            } else if (toread > remaining - avail) {
                toread = remaining - avail;
            }
        }
        if (toread == 0) return 0;

        int retval = connRead(r->io.conn.conn, (char *)r->io.conn.buf + sdslen(r->io.conn.buf), toread);
        if (retval == 0) {
            return 0;
        } else if (retval < 0) {
            if (connLastErrorRetryable(r->io.conn.conn)) continue;
            if (errno == EWOULDBLOCK) errno = ETIMEDOUT;
            return -1;
        }
        sdsIncrLen(r->io.conn.buf, retval);
        avail = sdslen(r->io.conn.buf) - r->io.conn.pos;
    }

    return 1;
}

/* Partial-read variant: returns bytes read, 0 on EOF, -1 on error. */
static ssize_t rioConnReadSome(rio *r, void *buf, size_t len) {
    size_t avail = sdslen(r->io.conn.buf) - r->io.conn.pos;

    if (r->io.conn.read_limit != 0 && r->io.conn.read_so_far >= r->io.conn.read_limit) {
        return 0;
    }
    if (avail == 0) {
        int fill_rc = rioConnEnsureBuffered(r, 1);
        if (fill_rc <= 0) return fill_rc;
        avail = sdslen(r->io.conn.buf) - r->io.conn.pos;
    }
    if (r->io.conn.read_limit != 0) {
        size_t remaining = r->io.conn.read_limit - r->io.conn.read_so_far;
        if (len > remaining) len = remaining;
    }

    size_t got = avail < len ? avail : len;
    memcpy(buf, (char *)r->io.conn.buf + r->io.conn.pos, got);
    r->io.conn.read_so_far += got;
    r->io.conn.pos += got;
    return (ssize_t)got;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioConnRead(rio *r, void *buf, size_t len) {
    if (r->io.conn.read_limit != 0 &&
        (r->io.conn.read_so_far > r->io.conn.read_limit ||
         len > r->io.conn.read_limit - r->io.conn.read_so_far)) {
        errno = EOVERFLOW;
        return 0;
    }
    if (rioConnEnsureBuffered(r, len) != 1) return 0;

    memcpy(buf, (char *)r->io.conn.buf + r->io.conn.pos, len);
    r->io.conn.read_so_far += len;
    r->io.conn.pos += len;
    return len;
}

/* Returns read/write position in file. */
static off_t rioConnTell(rio *r) {
    return r->io.conn.read_so_far;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioConnFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioConnWrite(r, NULL, 0);
}

static const rio rioConnIO = {
    .read = rioConnRead,
    .write = rioConnWrite,
    .tell = rioConnTell,
    .flush = rioConnFlush,
    .read_some = rioConnReadSome,
    .update_cksum = NULL,
    .cksum = 0,
    .flags = 0,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {{NULL, 0}},
};

/* Create an RIO that implements a buffered read from an fd
 * read_limit argument stops buffering when the reaching the limit. */
void rioInitWithConn(rio *r, connection *conn, size_t read_limit) {
    *r = rioConnIO;
    r->io.conn.conn = conn;
    r->io.conn.pos = 0;
    r->io.conn.read_limit = read_limit;
    r->io.conn.read_so_far = 0;
    r->io.conn.buf = sdsnewlen(NULL, PROTO_IOBUF_LEN);
    sdsclear(r->io.conn.buf);
}

/* Release the RIO stream. Optionally returns the unread buffered data
 * when the SDS pointer 'remaining' is passed. */
void rioFreeConn(rio *r, sds *remaining) {
    if (remaining && (size_t)r->io.conn.pos < sdslen(r->io.conn.buf)) {
        if (r->io.conn.pos > 0) sdsrange(r->io.conn.buf, r->io.conn.pos, -1);
        *remaining = r->io.conn.buf;
    } else {
        sdsfree(r->io.conn.buf);
        if (remaining) *remaining = NULL;
    }
    r->io.conn.buf = NULL;
}

/* ------------------- File descriptor implementation ------------------
 * This target is used to write the RDB file to pipe, when the primary just
 * streams the data to the replicas without creating an RDB on-disk image
 * (diskless replication option).
 * It only implements writes. */

/* Returns 1 or 0 for success/failure.
 *
 * When buf is NULL and len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioFdFlush(). */
static size_t rioFdWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    unsigned char *p = (unsigned char *)buf;
    int doflush = (buf == NULL && len == 0);

    /* For small writes, we rather keep the data in user-space buffer, and flush
     * it only when it grows. however for larger writes, we prefer to flush
     * any preexisting buffer, and write the new one directly without reallocs
     * and memory copying. */
    if (len > PROTO_IOBUF_LEN) {
        /* First, flush any preexisting buffered data. */
        if (sdslen(r->io.fd.buf)) {
            if (rioFdWrite(r, NULL, 0) == 0) return 0;
        }
        /* Write the new data, keeping 'p' and 'len' from the input. */
    } else {
        if (len) {
            r->io.fd.buf = sdscatlen(r->io.fd.buf, buf, len);
            if (sdslen(r->io.fd.buf) > PROTO_IOBUF_LEN) doflush = 1;
            if (!doflush) return 1;
        }
        /* Flushing the buffered data. set 'p' and 'len' accordingly. */
        p = (unsigned char *)r->io.fd.buf;
        len = sdslen(r->io.fd.buf);
    }

    size_t nwritten = 0;
    while (nwritten != len) {
        retval = write(r->io.fd.fd, p + nwritten, len - nwritten);
        if (retval <= 0) {
            if (retval == -1 && errno == EINTR) continue;
            /* With blocking io, which is the sole user of this
             * rio target, EWOULDBLOCK is returned only because of
             * the SO_SNDTIMEO socket option, so we translate the error
             * into one more recognizable by the user. */
            if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
            return 0; /* error. */
        }
        nwritten += retval;
    }

    r->io.fd.pos += len;
    sdsclear(r->io.fd.buf);
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioFdRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
static off_t rioFdTell(rio *r) {
    return r->io.fd.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFdFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioFdWrite(r, NULL, 0);
}

static const rio rioFdIO = {
    .read = rioFdRead,
    .write = rioFdWrite,
    .tell = rioFdTell,
    .flush = rioFdFlush,
    .read_some = NULL,
    .update_cksum = NULL,
    .cksum = 0,
    .flags = 0,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {{NULL, 0}},
};

void rioInitWithFd(rio *r, int fd) {
    *r = rioFdIO;
    r->io.fd.fd = fd;
    r->io.fd.pos = 0;
    r->io.fd.buf = sdsempty();
}

/* release the rio stream. */
void rioFreeFd(rio *r) {
    sdsfree(r->io.fd.buf);
}

/* ---------------------------- Generic functions ---------------------------- */

/* This function can be installed both in memory and file streams when checksum
 * computation is needed. */
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
    if ((r->flags & RIO_FLAG_SKIP_RDB_CHECKSUM) != 0) return; // skip RDB checksum
    r->cksum = crc64(r->cksum, buf, len);
}

/* Set the file-based rio object to auto-fsync every 'bytes' file written.
 * By default this is set to zero that means no automatic file sync is
 * performed.
 *
 * This feature is useful in a few contexts since when we rely on OS write
 * buffers sometimes the OS buffers way too much, resulting in too many
 * disk I/O concentrated in very little time. When we fsync in an explicit
 * way instead the I/O pressure is more distributed across time. */
void rioSetAutoSync(rio *r, off_t bytes) {
    if (r->write != rioFileIO.write) return;
    r->io.file.autosync = bytes;
}

/* Set the file-based rio object to reclaim cache after every auto-sync.
 * In the Linux implementation POSIX_FADV_DONTNEED skips the dirty
 * pages, so if auto sync is unset this option will have no effect.
 *
 * This feature can reduce the cache footprint backed by the file. */
void rioSetReclaimCache(rio *r, int enabled) {
    r->io.file.reclaim_cache = enabled;
}

/* --------------------------- Higher level interface --------------------------
 *
 * The following higher level functions use lower level rio.c functions to help
 * generating the RESP for the Append Only File. */

/* Write multi bulk count in the format: "*<count>\r\n". */
size_t rioWriteBulkCount(rio *r, char prefix, long count) {
    char cbuf[128];
    int clen;

    cbuf[0] = prefix;
    clen = 1 + ll2string(cbuf + 1, sizeof(cbuf) - 1, count);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (rioWrite(r, cbuf, clen) == 0) return 0;
    return clen;
}

/* Write binary-safe string in the format: "$<count>\r\n<payload>\r\n". */
size_t rioWriteBulkString(rio *r, const char *buf, size_t len) {
    size_t nwritten;

    if ((nwritten = rioWriteBulkCount(r, '$', len)) == 0) return 0;
    if (len > 0 && rioWrite(r, buf, len) == 0) return 0;
    if (rioWrite(r, "\r\n", 2) == 0) return 0;
    return nwritten + len + 2;
}

/* Write a long long value in format: "$<count>\r\n<payload>\r\n". */
size_t rioWriteBulkLongLong(rio *r, long long l) {
    char lbuf[32];
    unsigned int llen;

    llen = ll2string(lbuf, sizeof(lbuf), l);
    return rioWriteBulkString(r, lbuf, llen);
}

/* Write a double value in the format: "$<count>\r\n<payload>\r\n" */
size_t rioWriteBulkDouble(rio *r, double d) {
    char dbuf[128];
    unsigned int dlen;
    dlen = fpconv_dtoa(d, dbuf);
    dbuf[dlen] = '\0';
    return rioWriteBulkString(r, dbuf, dlen);
}

/* Returns 1 or 0 for success/failure.
 * The function returns success as long as we are able to correctly write
 * to at least one file descriptor.
 *
 * When buf is NULL and len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioConnsetFlush(). */
static size_t rioConnsetWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    int j;
    unsigned char *p = (unsigned char *)buf;
    int doflush = (buf == NULL && len == 0);

    /* To start we always append to our buffer. If it gets larger than
     * a given size, we actually write to the sockets. */
    if (len) {
        r->io.connset.buf = sdscatlen(r->io.connset.buf, buf, len);
        len = 0; /* Prevent entering the while below if we don't flush. */
        if (sdslen(r->io.connset.buf) > PROTO_IOBUF_LEN) doflush = 1;
    }

    if (doflush) {
        p = (unsigned char *)r->io.connset.buf;
        len = sdslen(r->io.connset.buf);
    }

    /* Write in little chunks so that when there are big writes we
     * parallelize while the kernel is sending data in background to
     * the TCP socket. */
    while (len) {
        size_t count = len < RIO_CONNSET_WRITE_MAX_CHUNK_SIZE ? len : RIO_CONNSET_WRITE_MAX_CHUNK_SIZE;
        int broken = 0;
        for (j = 0; j < r->io.connset.numconns; j++) {
            if (r->io.connset.state[j] != 0) {
                /* Skip FDs already in error. */
                broken++;
                continue;
            }

            /* Make sure to write 'count' bytes to the socket regardless
             * of short writes. */
            size_t nwritten = 0;
            while (nwritten != count) {
                retval = connWrite(r->io.connset.conns[j], p + nwritten, count - nwritten);
                if (retval <= 0) {
                    /* With blocking sockets, which is the sole user of this
                     * rio target, EWOULDBLOCK is returned only because of
                     * the SO_SNDTIMEO socket option, so we translate the error
                     * into one more recognizable by the user. */
                    if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
                    break;
                }
                /* Count bytes actually written to this connection, including a
                 * partial write before an error. */
                r->io.connset.net_output_bytes += retval;
                nwritten += retval;
            }

            if (nwritten != count) {
                /* Mark this FD as broken. */
                r->io.connset.state[j] = errno;
                if (r->io.connset.state[j] == 0) r->io.connset.state[j] = EIO;
            }
        }
        if (broken == r->io.connset.numconns) return 0; /* All the FDs in error. */
        p += count;
        len -= count;
        r->io.connset.pos += count;
    }

    if (doflush) sdsclear(r->io.connset.buf);
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioConnsetRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
static off_t rioConnsetTell(rio *r) {
    return r->io.connset.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioConnsetFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioConnsetWrite(r, NULL, 0);
}

static const rio rioConnsetIO = {
    .read = rioConnsetRead,
    .write = rioConnsetWrite,
    .tell = rioConnsetTell,
    .flush = rioConnsetFlush,
    .read_some = NULL,
    .update_cksum = NULL,
    .cksum = 0,
    .flags = 0,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {{NULL, 0}},
};

void rioInitWithConnset(rio *r, connection **conns, int numconns) {
    *r = rioConnsetIO;
    r->io.connset.conns = zmalloc(sizeof(connection *) * numconns);
    r->io.connset.state = zmalloc(sizeof(int) * numconns);
    for (int i = 0; i < numconns; i++) {
        r->io.connset.conns[i] = conns[i];
        r->io.connset.state[i] = 0;
    }
    r->io.connset.numconns = numconns;
    r->io.connset.pos = 0;
    r->io.connset.buf = sdsempty();
    r->io.connset.net_output_bytes = 0;
}

/* release the rio stream. */
void rioFreeConnset(rio *r) {
    zfree(r->io.connset.conns);
    zfree(r->io.connset.state);
    sdsfree(r->io.connset.buf);
}

/* ---------------- Whole-stream compression implementation ----------------
 * A compression rio wraps a target rio. Logical bytes written to it are
 * compressed into the target, and logical bytes read from it are decoded from
 * the target. Each layer keeps its own error flags: the compression rio
 * latches a read or write error when the codec or the target fails, and
 * carries an asynchronous close of the target up to itself. */

static void rioCompressInheritErrors(rio *r) {
    r->flags |= r->io.compressed.target->flags & RIO_FLAG_CLOSE_ASAP;
}

/* streamWriter sink: encoded bytes go to the target. */
static int rioCompressWriteTarget(void *ctx, const uint8_t *data, size_t len) {
    return rioWrite((rio *)ctx, data, len) ? C_OK : C_ERR;
}

/* streamReader source: encoded bytes come from the target. The reader limits
 * each request to the codec's input hint, so it never reads past the frame. */
static ssize_t rioCompressReadTarget(void *ctx, void *buf, size_t len) {
    return rioReadSome((rio *)ctx, buf, len);
}

/* Returns 1 or 0 for success/failure. */
static size_t rioCompressWrite(rio *r, const void *buf, size_t len) {
    streamWriter *writer = r->io.compressed.writer;
    if (!writer) return 0; /* Error, a decompressing rio does not support writing. */
    if (streamWriterWrite(writer, buf, len) == C_OK) return 1;
    rioCompressInheritErrors(r);
    return 0;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioCompressRead(rio *r, void *buf, size_t len) {
    streamReader *reader = r->io.compressed.reader;
    if (r->io.compressed.writer) return 0; /* Error, a compressing rio does not support reading. */
    if (!reader) {
        /* Plain file input, rewound after the probe. */
        if (rioRead(r->io.compressed.target, buf, len)) return 1;
        rioCompressInheritErrors(r);
        return 0;
    }

    uint8_t *dst = (uint8_t *)buf;
    while (len > 0) {
        ssize_t nread = streamReaderRead(reader, dst, len);
        if (nread <= 0) {
            rioCompressInheritErrors(r);
            return 0;
        }
        dst += nread;
        len -= (size_t)nread;
    }
    return 1;
}

/* Partial-read variant: returns bytes read, 0 on EOF, -1 on error. */
static ssize_t rioCompressReadSome(rio *r, void *buf, size_t len) {
    streamReader *reader = r->io.compressed.reader;
    if (r->io.compressed.writer) return -1;
    ssize_t nread = reader ? streamReaderRead(reader, buf, len) : rioReadSome(r->io.compressed.target, buf, len);
    if (nread < 0) rioCompressInheritErrors(r);
    return nread;
}

/* Returns the read/write position in the target, i.e. in the encoded stream. */
static off_t rioCompressTell(rio *r) {
    return rioTell(r->io.compressed.target);
}

/* Flushes the target. Output the codec still buffers is emitted by later
 * writes or by rioFinishCompression(). */
static int rioCompressFlush(rio *r) {
    return rioFlush(r->io.compressed.target);
}

/* Logical writes abort whenever the target would abort, so an abort callback
 * installed on either layer is checked between logical chunks. */
static int rioCompressCheckAbort(rio *r) {
    rio *target = r->io.compressed.target;
    return target->check_abort_between_writes && target->check_abort_between_writes(target);
}

static const rio rioCompressIO = {
    .read = rioCompressRead,
    .write = rioCompressWrite,
    .tell = rioCompressTell,
    .flush = rioCompressFlush,
    .read_some = rioCompressReadSome,
    .update_cksum = NULL,
    .check_abort_between_writes = rioCompressCheckAbort,
    .cksum = 0,
    .flags = 0,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {{NULL, 0}},
};

static void rioInitCompressed(rio *r, rio *target) {
    *r = rioCompressIO;
    /* The RDB checksum policy of the target carries over. */
    r->flags = target->flags & RIO_FLAG_SKIP_RDB_CHECKSUM;
    r->io.compressed.target = target;
    r->io.compressed.writer = NULL;
    r->io.compressed.reader = NULL;
}

/* Compress the logical bytes written to 'r' into 'target' with 'algo'. Codec
 * integrity checksums follow rdbchecksum. If the codec cannot start, 'r' has
 * a write error from the start. End the frame with rioFinishCompression()
 * before writing anything else to the target, then release 'r' with
 * rioFreeCompression(). */
void rioInitWithCompression(rio *r, compressionAlgo algo, rio *target) {
    rioInitCompressed(r, target);
    r->flags |= RIO_FLAG_STREAMING_COMPRESSION;
    r->io.compressed.writer = zmalloc(sizeof(streamWriter));
    if (streamWriterInit(r->io.compressed.writer, algo, server.rdb_checksum, rioCompressWriteTarget, target) == C_ERR)
        r->flags |= RIO_FLAG_WRITE_ERROR;
}

/* Probe 'target' for a VCS envelope and set up 'r' to read the logical
 * stream. Compressed input is decoded, flagged with
 * RIO_FLAG_STREAMING_COMPRESSION. Plain input is passed through unchanged,
 * read from the target directly when it is a file the probe can be rewound
 * in. Codec checksums are verified unless rdbchecksum, checksum validation, or
 * the target's RDB checksum policy turns them off. A frame that ends early is
 * truncated when the target is a connection that can deliver the rest later,
 * and corrupt otherwise.
 *
 * Returns C_OK, or C_ERR when the envelope is invalid or unsupported or the
 * codec cannot start; the reader's error_kind classifies the failure. Either
 * way, release 'r' with rioFreeCompression(). */
int rioInitWithDecompression(rio *r, rio *target) {
    rioInitCompressed(r, target);
    streamReaderConfig cfg = {
        .allow_passthrough = true,
        .skip_codec_checksum_validation = (target->flags & RIO_FLAG_SKIP_RDB_CHECKSUM) || !server.rdb_checksum ||
                                          server.skip_checksum_validation,
        .buffer_size = STREAM_READER_BUFFER_SIZE_DEFAULT,
        .eof_mid_frame_is_truncation = (rioCheckType(target) == RIO_TYPE_CONN),
    };
    streamReader *reader = zmalloc(sizeof(*reader));
    r->io.compressed.reader = reader;

    compressionAlgo algo = ALGO_NONE;
    if (streamReaderInit(reader, &cfg, rioCompressReadTarget, target, &algo) == C_ERR) {
        r->flags |= RIO_FLAG_READ_ERROR;
        return C_ERR;
    }
    if (algo != ALGO_NONE) {
        r->flags |= RIO_FLAG_STREAMING_COMPRESSION;
        return C_OK;
    }

    /* Replay the probe of a plain file through the file itself, keeping plain
     * RDB loads off the passthrough path while retaining it for sources that
     * cannot be rewound. */
    size_t probe_len = reader->probe.header_len;
    if (rioCheckType(target) == RIO_TYPE_FILE && probe_len <= target->processed_bytes &&
        fseeko(target->io.file.fp, -(off_t)probe_len, SEEK_CUR) == 0) {
        target->processed_bytes -= probe_len;
        streamReaderFree(reader);
        zfree(reader);
        r->io.compressed.reader = NULL;
    }
    return C_OK;
}

/* End the compressed frame. A compressing rio writes the frame trailer to the
 * target without flushing it; repeated calls after a success are no-ops. A
 * decompressing rio consumes and validates the rest of the frame without
 * reading past it, leaving the target at the first byte after the frame; on
 * failure the reader's error_kind classifies it. Returns 1 on success and 0
 * on failure, or when an earlier operation already failed. */
int rioFinishCompression(rio *r) {
    serverAssert(rioCheckType(r) == RIO_TYPE_COMPRESSED);
    if (r->io.compressed.writer) {
        if (r->flags & RIO_FLAG_WRITE_ERROR || r->flags & RIO_FLAG_CLOSE_ASAP) return 0;
        if (streamWriterFinish(r->io.compressed.writer) == C_OK) return 1;
        r->flags |= RIO_FLAG_WRITE_ERROR;
    } else {
        if (r->flags & RIO_FLAG_READ_ERROR || r->flags & RIO_FLAG_CLOSE_ASAP) return 0;
        if (!r->io.compressed.reader || streamReaderFinish(r->io.compressed.reader) == C_OK) return 1;
        r->flags |= RIO_FLAG_READ_ERROR;
    }
    rioCompressInheritErrors(r);
    return 0;
}

/* Release the codec engine. The target is left to the caller. */
void rioFreeCompression(rio *r) {
    if (r->io.compressed.writer) {
        streamWriterFree(r->io.compressed.writer);
        zfree(r->io.compressed.writer);
        r->io.compressed.writer = NULL;
    }
    if (r->io.compressed.reader) {
        streamReaderFree(r->io.compressed.reader);
        zfree(r->io.compressed.reader);
        r->io.compressed.reader = NULL;
    }
}

/* The codec carrying the logical stream, or ALGO_NONE for plain input. */
compressionAlgo rioCompressionAlgo(rio *r) {
    if (rioCheckType(r) != RIO_TYPE_COMPRESSED || !(r->flags & RIO_FLAG_STREAMING_COMPRESSION)) return ALGO_NONE;
    if (r->io.compressed.writer) return r->io.compressed.writer->compressor.algo;
    return r->io.compressed.reader->decompressor.algo;
}

/* Check the type of rio. */
uint8_t rioCheckType(rio *r) {
    if (r->read == rioFileRead) {
        return RIO_TYPE_FILE;
    } else if (r->read == rioBufferRead) {
        return RIO_TYPE_BUFFER;
    } else if (r->read == rioConnRead) {
        return RIO_TYPE_CONN;
    } else if (r->read == rioCompressRead) {
        return RIO_TYPE_COMPRESSED;
    } else {
        /* r->read == rioFdRead */
        return RIO_TYPE_FD;
    }
}
