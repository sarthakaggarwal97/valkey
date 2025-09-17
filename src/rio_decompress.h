#ifndef VALKEY_RIO_DECOMPRESS_H
#define VALKEY_RIO_DECOMPRESS_H

#include <stddef.h>
#include <stdint.h>
#include "rio.h"

#define RDBFRAME_MAGIC "RDBF"
#define RDBFRAME_MAGIC_LEN 4
#define RDBFRAME_VERSION 1
#define RDBFRAME_HEADER_LEN 24

#define RDBFRAME_FLAG_LAST (1 << 0)

typedef enum {
    RDBFRAME_TYPE_RAW = 0,
    RDBFRAME_TYPE_LZF = 1,
} RdbFrameBlockType;

typedef struct RdbFrameBlockHdr {
    uint8_t magic[RDBFRAME_MAGIC_LEN];
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t reserved;
    uint32_t payload_len;
    uint32_t raw_len;
    uint64_t payload_crc;
} RdbFrameBlockHdr;

typedef struct rio_decompress {
    rio *src;
    sds rawbuf;
    size_t pos;
    int eof;
    rio rio_itf;
} rio_decompress;

int rioInitDecompress(rio_decompress *rd, rio *src);

#endif /* VALKEY_RIO_DECOMPRESS_H */
