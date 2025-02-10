#include "compression.h"
#include "lzf.h"
#include "lz4.h"
#include "server.h"

#define UNUSED(x) (void)(x)

size_t lz4_compress(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    return LZ4_compress_default((char *)in_data, out_data, in_len, out_len);
}

size_t lz4_decompress(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    return LZ4_decompress_safe((char *)in_data, out_data, in_len, out_len);
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
