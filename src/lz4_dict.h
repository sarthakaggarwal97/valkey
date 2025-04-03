#ifndef LZ4_DICT_H
#define LZ4_DICT_H

#include "lz4.h"
#include "zmalloc.h"
#include <string.h>

/* Custom compression and decompression functions that use dictionary‐based, stateful API. */
size_t lz4_compress_using_dict(const void *in_data, size_t in_len, void *out_data, size_t out_len);
size_t lz4_decompress_using_dict(const void *in_data, size_t in_len, void *out_data, size_t out_len);

#endif /* LZ4_DICT_H */
