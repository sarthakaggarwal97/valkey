#include "compression.h"
#include "lzf.h"
#include "lz4.h"
#include "server.h"

#define UNUSED(x) (void)(x)

#define DICT_SIZE 65536  // 64 KB dictionary

static char compression_dict[DICT_SIZE];
static int dict_loaded = 0;

// Persistent LZ4 stream states
static LZ4_stream_t lz4Stream;
static LZ4_streamDecode_t lz4StreamDecode;

/* Initialize the dictionary (if not already initialized) */
void initLZ4Dictionary() {
    if (!dict_loaded) {
        memset(compression_dict, 'A', DICT_SIZE);  // Example: Fill with dummy data
        dict_loaded = 1;
        serverLog(LL_NOTICE, "LZ4 dictionary initialized.");
    }
}

/* LZ4 Compression with a dictionary */
size_t lz4_compress(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    serverLog(LL_NOTICE, "LZ4 compressing with dictionary");

    if (!dict_loaded) initLZ4Dictionary();  // Ensure dictionary is initialized

    // Load dictionary into the LZ4 stream state
    LZ4_loadDict(&lz4Stream, compression_dict, DICT_SIZE);

    // Compress using dictionary
    int compressed_size = LZ4_compress_fast_continue(
        &lz4Stream, (const char *)in_data, (char *)out_data, in_len, out_len, 1);

    if (compressed_size <= 0) {
        serverLog(LL_WARNING, "LZ4 compression failed!");
        return 0;
    }

    // Save the last 64 KB as the rolling dictionary
    LZ4_saveDict(&lz4Stream, compression_dict, DICT_SIZE);

    return compressed_size;
}

/* LZ4 Decompression with a dictionary */
size_t lz4_decompress(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    serverLog(LL_NOTICE, "LZ4 decompressing with dictionary");

    if (!dict_loaded) initLZ4Dictionary();  // Ensure dictionary is initialized

    // Load dictionary into decompression state
    LZ4_setStreamDecode(&lz4StreamDecode, compression_dict, DICT_SIZE);

    // Decompress using dictionary
    int decompressed_size = LZ4_decompress_safe_usingDict(
        (const char *)in_data, (char *)out_data, in_len, out_len, compression_dict, DICT_SIZE);

    if (decompressed_size < 0) {
        serverLog(LL_WARNING, "LZ4 decompression failed!");
        return 0;
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
