#include "lz4_dict.h"
#include "server.h"

static char dictBuffer[64 * 1024] = {0};  // 64KB dictionary buffer
static size_t dictSize = 0;               // Tracks dictionary size

/* Custom compression function using stateful API.
 * We create a temporary LZ4_stream_t, load the dictionary, compress using
 * LZ4_compress_fast_continue, then free the state.
 */
size_t lz4_compress_using_dict(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    LZ4_stream_t *lz4Stream = LZ4_createStream();
    if (!lz4Stream) return 0;

    LZ4_loadDict(lz4Stream, dictBuffer, dictSize);
    int compressed = LZ4_compress_fast_continue(lz4Stream, (const char *)in_data, (char *)out_data, in_len, out_len, 8);

    LZ4_freeStream(lz4Stream);
    return (compressed > 0) ? (size_t)compressed : 0;
}

/* Custom decompression function using stateful API.
 * Create a LZ4_streamDecode_t, set the dictionary, decompress with LZ4_decompress_safe_continue,
 * then free the state.
 */
size_t lz4_decompress_using_dict(const void *in_data, size_t in_len, void *out_data, size_t out_len) {
    LZ4_streamDecode_t *lz4StreamDecode = LZ4_createStreamDecode();
    if (!lz4StreamDecode) return 0;

    LZ4_setStreamDecode(lz4StreamDecode, dictBuffer, dictSize);
    int decompressed = LZ4_decompress_safe_continue(lz4StreamDecode, (const char *)in_data, (char *)out_data, in_len, out_len);

    LZ4_freeStreamDecode(lz4StreamDecode);
    return (decompressed > 0) ? (size_t)decompressed : 0;
}

