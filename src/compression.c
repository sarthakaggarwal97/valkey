#include "compression.h"

#include <sys/time.h>

#include "lzf.h"
#include "lz4.h"
#include "server.h"

/* LZ4 compress with logging for debugging.
 * Logs:
 *  - Input size
 *  - Compressed size
 *  - Compression ratio
 *  - Time taken in microseconds
 */
size_t lz4_compress(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    struct timeval start, end;
    gettimeofday(&start, NULL);

    size_t compressed_size = LZ4_compress_default((char *)in_data, out_data, in_len, out_len);

    gettimeofday(&end, NULL);
    long elapsed_usec = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_usec - start.tv_usec);

    if (compressed_size > 0) {
        double ratio = (double)compressed_size / (double)in_len;
        serverLog(LL_NOTICE,
                  "LZ4 compress: input=%zu bytes, compressed=%zu bytes, ratio=%.2f, time=%ld usec",
                  in_len, compressed_size, ratio, elapsed_usec);
    } else {
        serverLog(LL_WARNING,
                  "LZ4 compress failed: input=%zu bytes, output buffer=%zu bytes, time=%ld usec",
                  in_len, out_len, elapsed_usec);
    }
    return compressed_size;
}

/* LZ4 decompress with logging for debugging.
 * Logs:
 *  - Compressed input size
 *  - Decompressed output size
 *  - Time taken in microseconds
 */
size_t lz4_decompress(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    struct timeval start, end;
    gettimeofday(&start, NULL);

    size_t decompressed_size = LZ4_decompress_safe((char *)in_data, out_data, in_len, out_len);

    gettimeofday(&end, NULL);
    long elapsed_usec = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_usec - start.tv_usec);

    if (decompressed_size > 0) {
        serverLog(LL_NOTICE,
                  "LZ4 decompress: compressed input=%zu bytes, decompressed=%zu bytes, time=%ld usec",
                  in_len, decompressed_size, elapsed_usec);
    } else {
        serverLog(LL_WARNING,
                  "LZ4 decompress failed: compressed input=%zu bytes, output buffer=%zu bytes, time=%ld usec",
                  in_len, out_len, elapsed_usec);
    }
    return decompressed_size;
}

static CompressionType CompressionType_LZ4 = {
    .name = COMP_TYPE_LZ4,
    .compress = lz4_compress,
    .decompress = lz4_decompress,
    .encode_value = RDB_ENC_LZ4
};

static CompressionType CompressionType_LZF = {
    .name = COMP_TYPE_LZF,
    .compress = lzf_compress,
    .decompress = lzf_decompress,
    .encode_value = RDB_ENC_LZF
};

CompressionType *compressionTypeLZF(void) {
    return &CompressionType_LZF;
}

CompressionType *compressionTypeLZ4(void) {
    return &CompressionType_LZ4;
}
