/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_RIO_H
#define COMPRESSION_RIO_H

#include "compression_stream.h"
#include "rio.h"

/* RIO decorators for VCS-framed whole-stream RDB compression.
 *
 *   save: RDB -> compression RIO -> inner RIO
 *   load: RDB <- decompression RIO <- inner RIO
 *
 * Both decorators are ordinary rio values. Their codec state is private and
 * their inner RIO is borrowed. rioFinish() completes a successful decorator
 * chain; rioFree() releases only the selected decorator's private state. */
typedef enum {
    RIO_DECOMPRESSION_INIT_ERROR = -1,
    RIO_DECOMPRESSION_INIT_OK = 0,
    RIO_DECOMPRESSION_INIT_INCOMPATIBLE = 1,
} rioDecompressionInitResult;

int rioInitWithCompression(rio *r,
                           compressionAlgo algo,
                           rio *inner,
                           bool codec_checksum);
rioDecompressionInitResult rioInitWithDecompression(rio *r,
                                                    rio *inner,
                                                    bool skip_codec_checksum_validation,
                                                    compressionAlgo *algo);
streamReaderErrorKind rioDecompressionGetError(const rio *r);
size_t rioDecompressionTakeUnreportedBytes(rio *r);

#endif /* COMPRESSION_RIO_H */
