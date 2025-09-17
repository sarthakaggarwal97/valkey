#include <string.h>

#include "test_help.h"
#include "../crc64.h"
#include "../endianconv.h"
#include "../lzf.h"
#include "../rio.h"
#include "../rio_decompress.h"
#include "../sds.h"
#include "../zmalloc.h"

static sds buildRawFixture(void) {
    const size_t compressible = 512;
    const size_t incompressible = 256;
    sds raw = sdsnewlen(NULL, compressible + incompressible);
    TEST_ASSERT(raw != NULL);
    memset(raw, 'A', compressible);
    for (size_t i = 0; i < incompressible; i++) {
        raw[compressible + i] = (unsigned char)((i * 37u) & 0xffu);
    }
    return raw;
}

static sds buildFramedStream(const unsigned char *raw, size_t raw_len, size_t chunk_len) {
    sds stream = sdsempty();
    TEST_ASSERT(stream != NULL);
    size_t pos = 0;
    while (pos < raw_len) {
        size_t block_len = chunk_len;
        if (block_len > raw_len - pos) block_len = raw_len - pos;
        const unsigned char *block = raw + pos;

        unsigned char *compressed = NULL;
        unsigned int compressed_len = 0;
        if (block_len) {
            size_t max_compress = block_len + block_len / 16 + 64 + 3;
            compressed = zmalloc(max_compress ? max_compress : 1);
            TEST_ASSERT(compressed != NULL);
            compressed_len = lzf_compress(block, block_len, compressed, max_compress);
        }

        int type = RDBFRAME_TYPE_RAW;
        const unsigned char *payload = block;
        size_t payload_len = block_len;
        if (compressed_len > 0 && compressed_len < block_len) {
            type = RDBFRAME_TYPE_LZF;
            payload = compressed;
            payload_len = compressed_len;
        }

        unsigned char hdr[RDBFRAME_HEADER_LEN];
        memcpy(hdr, RDBFRAME_MAGIC, RDBFRAME_MAGIC_LEN);
        hdr[4] = RDBFRAME_VERSION;
        hdr[5] = type;
        hdr[6] = (pos + block_len == raw_len) ? RDBFRAME_FLAG_LAST : 0;
        hdr[7] = 0;

        uint32_t payload_store = payload_len;
        memrev32ifbe(&payload_store);
        memcpy(hdr + 8, &payload_store, sizeof(payload_store));

        uint32_t raw_store = block_len;
        memrev32ifbe(&raw_store);
        memcpy(hdr + 12, &raw_store, sizeof(raw_store));

        uint64_t crc = crc64(0, payload_len ? payload : (const unsigned char *)"", payload_len);
        uint64_t crc_store = crc;
        memrev64ifbe(&crc_store);
        memcpy(hdr + 16, &crc_store, sizeof(crc_store));

        stream = sdscatlen(stream, hdr, sizeof(hdr));
        TEST_ASSERT(stream != NULL);
        if (payload_len) {
            stream = sdscatlen(stream, payload, payload_len);
            TEST_ASSERT(stream != NULL);
        }

        if (compressed) zfree(compressed);
        pos += block_len;
    }

    return stream;
}

int test_rioDecompressBasic(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    sds raw = buildRawFixture();
    size_t raw_len = sdslen(raw);
    sds framed = buildFramedStream((unsigned char *)raw, raw_len, 128);

    rio src;
    rioInitWithBuffer(&src, framed);

    rio_decompress rd;
    TEST_ASSERT(rioInitDecompress(&rd, &src));

    char *out = zmalloc(raw_len);
    TEST_ASSERT(out != NULL);

    rd.rio_itf.max_processing_chunk = 31;
    TEST_ASSERT(rioRead(&rd.rio_itf, out, raw_len));
    TEST_ASSERT(memcmp(out, raw, raw_len) == 0);

    zfree(out);
    sdsfree(rd.rawbuf);
    sdsfree(framed);
    sdsfree(raw);
    return 0;
}
