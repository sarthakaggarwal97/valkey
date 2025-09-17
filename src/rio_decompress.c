#include "rio_decompress.h"

#include <stddef.h>
#include <string.h>

#include "crc64.h"
#include "endianconv.h"
#include "lzf.h"
#include "sds.h"
#include "zmalloc.h"

#ifndef UNUSED
#define UNUSED(V) ((void)(V))
#endif

#define RDBFRAME_RESERVED_EXPECTED 0

static inline rio_decompress *rioDecompressFromRio(rio *r) {
    return (rio_decompress *)((unsigned char *)r - offsetof(rio_decompress, rio_itf));
}

static int rioDecompressError(rio_decompress *rd) {
    rd->rio_itf.flags |= RIO_FLAG_READ_ERROR;
    return 0;
}

static int rioDecompressEnsureRawbuf(rio_decompress *rd, size_t raw_len) {
    if (rd->rawbuf == NULL) {
        rd->rawbuf = sdsempty();
        if (rd->rawbuf == NULL) return 0;
    }
    sdssetlen(rd->rawbuf, 0);
    if (raw_len == 0) return 1;
    sds newbuf = sdsMakeRoomFor(rd->rawbuf, raw_len);
    if (newbuf == NULL) return 0;
    rd->rawbuf = newbuf;
    return 1;
}

static int rioDecompressLoadBlock(rio_decompress *rd) {
    unsigned char hdr_buf[RDBFRAME_HEADER_LEN];
    if (!rioRead(rd->src, hdr_buf, sizeof(hdr_buf))) {
        return rioDecompressError(rd);
    }

    if (memcmp(hdr_buf, RDBFRAME_MAGIC, RDBFRAME_MAGIC_LEN) != 0) {
        return rioDecompressError(rd);
    }

    uint8_t version = hdr_buf[4];
    uint8_t type = hdr_buf[5];
    uint8_t flags = hdr_buf[6];
    uint8_t reserved = hdr_buf[7];

    if (version != RDBFRAME_VERSION) {
        return rioDecompressError(rd);
    }
    if (reserved != RDBFRAME_RESERVED_EXPECTED) {
        return rioDecompressError(rd);
    }

    uint32_t payload_len32;
    memcpy(&payload_len32, hdr_buf + 8, sizeof(payload_len32));
    memrev32ifbe(&payload_len32);
    size_t payload_len = payload_len32;

    uint32_t raw_len32;
    memcpy(&raw_len32, hdr_buf + 12, sizeof(raw_len32));
    memrev32ifbe(&raw_len32);
    size_t raw_len = raw_len32;

    uint64_t payload_crc64;
    memcpy(&payload_crc64, hdr_buf + 16, sizeof(payload_crc64));
    memrev64ifbe(&payload_crc64);

    if (type != RDBFRAME_TYPE_RAW && type != RDBFRAME_TYPE_LZF) {
        return rioDecompressError(rd);
    }
    if (type == RDBFRAME_TYPE_RAW && raw_len != payload_len) {
        return rioDecompressError(rd);
    }

    unsigned char *payload = NULL;
    if (payload_len) {
        payload = zmalloc(payload_len);
        if (payload == NULL) return rioDecompressError(rd);
        if (!rioRead(rd->src, payload, payload_len)) {
            zfree(payload);
            return rioDecompressError(rd);
        }
    }

    const unsigned char empty = 0;
    const unsigned char *payload_ptr = payload_len ? payload : &empty;
    uint64_t crc = crc64(0, payload_ptr, payload_len);
    if (crc != payload_crc64) {
        if (payload) zfree(payload);
        return rioDecompressError(rd);
    }

    if (!rioDecompressEnsureRawbuf(rd, raw_len)) {
        if (payload) zfree(payload);
        return rioDecompressError(rd);
    }

    if (type == RDBFRAME_TYPE_RAW) {
        if (raw_len) memcpy(rd->rawbuf, payload, raw_len);
    } else {
        if (raw_len) {
            unsigned int produced = lzf_decompress(payload, payload_len, rd->rawbuf, raw_len);
            if (produced != raw_len) {
                if (payload) zfree(payload);
                return rioDecompressError(rd);
            }
        }
    }

    sdssetlen(rd->rawbuf, raw_len);
    rd->pos = 0;
    rd->eof = (flags & RDBFRAME_FLAG_LAST) != 0;

    if (payload) zfree(payload);
    return 1;
}

static size_t rioDecompressRead(rio *r, void *buf, size_t len) {
    if (len == 0) return 1;
    rio_decompress *rd = rioDecompressFromRio(r);
    unsigned char *p = buf;
    size_t remaining = len;

    while (remaining) {
        size_t available = sdslen(rd->rawbuf) - rd->pos;
        if (available == 0) {
            if (rd->eof) return 0;
            if (!rioDecompressLoadBlock(rd)) return 0;
            available = sdslen(rd->rawbuf) - rd->pos;
            if (available == 0) {
                if (rd->eof) return 0;
                continue;
            }
        }
        size_t tocopy = available < remaining ? available : remaining;
        memcpy(p, rd->rawbuf + rd->pos, tocopy);
        rd->pos += tocopy;
        p += tocopy;
        remaining -= tocopy;
    }
    return 1;
}

static size_t rioDecompressWrite(rio *r, const void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0;
}

static off_t rioDecompressTell(rio *r) {
    return r->processed_bytes;
}

static int rioDecompressFlush(rio *r) {
    UNUSED(r);
    return 1;
}

int rioInitDecompress(rio_decompress *rd, rio *src) {
    if (!rd || !src) return 0;
    memset(rd, 0, sizeof(*rd));
    rd->src = src;
    rd->rawbuf = sdsempty();
    if (rd->rawbuf == NULL) return 0;
    rd->pos = 0;
    rd->eof = 0;

    rd->rio_itf.read = rioDecompressRead;
    rd->rio_itf.write = rioDecompressWrite;
    rd->rio_itf.tell = rioDecompressTell;
    rd->rio_itf.flush = rioDecompressFlush;
    rd->rio_itf.update_cksum = NULL;
    rd->rio_itf.cksum = 0;
    rd->rio_itf.flags = 0;
    rd->rio_itf.processed_bytes = 0;
    rd->rio_itf.max_processing_chunk = 0;
    memset(&rd->rio_itf.io, 0, sizeof(rd->rio_itf.io));
    return 1;
}
