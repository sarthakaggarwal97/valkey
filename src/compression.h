#ifndef __COMPRESSION_H
#define __COMPRESSION_H

#include <stddef.h>

#define COMP_TYPE_LZF "lzf"
#define COMP_TYPE_LZ4 "lz4"

typedef struct CompressionType {
    /* compression type name */
    const char *name;

    /* method for compression */
    size_t (*compress)(const void *in_data, size_t in_len, void *out_data, size_t out_len);

    /* method for decompression */
    size_t (*decompress)(const void *in_data, size_t in_len, void *out_data, size_t out_len);

    /* encoded value */
    int encode_value;
} CompressionType;

CompressionType *compressionTypeLZF(void);

CompressionType *compressionTypeLZ4(void);

#endif
