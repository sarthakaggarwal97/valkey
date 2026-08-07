# PR #4075 review draft

Draft review text for manual use. These comments have not been posted to `valkey-io/valkey`.

- Repository: `valkey-io/valkey`
- PR: `#4075` — Streaming Compression support for fullsync
- Reviewed head: `5d85734189f1df89f006f136b6b58e0d58091bb6`
- Intended review event: `REQUEST_CHANGES`
- Revalidated: 2026-08-07

## Top-level review

Thanks @roshkhatri. I took a look at the latest version. The compression and negotiation design looks reasonable, but I think we need to fix a few correctness issues before merging.

Codec corruption found while finishing the frame is currently handled like a normal load failure, which can put the replica into a full-sync retry loop. We can also lose encoded bytes from `total_net_repl_input_bytes` when the reader consumes data during initialization or finalization. The current tests do not exercise either production failure mode.

The config and capability names in the code do not match the PR and issue text, and the branch still includes a divergent copy of the unmerged #3531. I also see five failing CI jobs at this head: `test-ubuntu-no-malloc-usable-size`, `test-ubuntu-io-threads`, `test-ubuntu-tls-io-threads`, `test-fedorarawhide-tls-module` and `test-macos-latest`.

Can we fix the finalization handling and byte accounting, add the missing tests, align the public contract, rebase after #3531 lands and get CI green?

## Inline comment 1

- Path: `src/replication.c`
- Line: `2583`
- Side: `RIGHT`

I think codec corruption found here bypasses the fatal RDB corruption path. If the decoder fails while parsing, it reaches `rdbReportCorruptRDB()`. If parsing returns `RDB_OK` and the checksum error is found by `streamReaderFinish()`, we only set `loadingFailed` and reconnect.

This is reachable when the decoded RDB ends exactly on the 1 MiB reader-buffer boundary and the LZ4 checksum footer is left for `streamReaderFinish()`. I reproduced it with a mutated final checksum byte: the reader reports `CORRUPT`, but the replica retries instead of taking the fatal corruption path. A primary that keeps sending the same corrupt stream can cause an endless full-sync loop.

Can we route `STREAM_READER_ERROR_CORRUPT` through the same corruption handler from both loaders, while keeping truncation and transport errors recoverable?

## Inline comment 2

- Path: `src/replication.c`
- Line: `2591`
- Side: `RIGHT`

I think we can miss encoded bytes from `total_net_repl_input_bytes` when the stream reader is detached or freed. `rdbLoadProgressCallback()` reports the encoded delta only after a successful decoded `rioRead()`, but the reader also consumes input while probing in `rdbInitStreamReader()` and while finishing the frame.

I reproduced a 4-byte undercount on a successful 4 MiB incompressible transfer, and a 49,164-byte undercount on a finish-time failure. Can we flush the residual encoded-byte delta before every detach or cleanup, including initialization and finalization failures? We should be careful to leave the plaintext EOF framing on the raw accounting path so it is not counted twice.

## Inline comment 3

- Path: `src/config.c`
- Line: `3461`
- Side: `RIGHT`

Can we align the public contract here? The implementation uses `repl-compress-sync` and `capa compress-sync`, while the PR body and issue #3195 still say `repl-compression` and `capa compression`. The REPLCONF capability comment also does not list the new capability.

The description of #3531 does not match the current trailing-byte and `rdbchecksum` behavior either. I don't see a linked `valkey-doc` PR or a `needs-doc-pr` label. I think the code comments, PR and issue text, tests and docs should all use the implemented names and semantics.

## Inline comment 4

- Path: `tests/integration/repl-fullsync-compression.tcl`
- Line: `475`
- Side: `RIGHT`

I don't think corrupt-stream coverage should be left as a follow-up. The low-level test checks the `CORRUPT` classification, but neither production full-sync caller tests what happens when corruption is found during `streamReaderFinish()`. That is why the current tests pass even though this gets downgraded to a retry.

Can we add deterministic tests that mutate an LZ4 checksum footer when the decoded RDB ends on the reader-buffer boundary? We should cover both socket and file loading, assert the same fatal behavior as parser-time corruption and keep the existing truncation case to verify that a clean mid-frame EOF still retries.

## Inline comment 5

- Path: `tests/integration/repl-fullsync-compression.tcl`
- Line: `796`
- Side: `RIGHT`

I don't think this assertion can catch the missing-byte issue. It only checks that the compressed counter is less than half of the plaintext counter, so it still passes if footer bytes are dropped or a failed sync loses a larger residual.

Can we test the production socket-load path with exact accounting? I think we need one success case that ends on the reader-buffer boundary and one finish-time failure, and both should verify that every encoded byte consumed by the stream reader reaches the replication input counter.

## Inline comment 6

- Path: `deps/lz4/Makefile`
- Line: `19`
- Side: `RIGHT`

Should `lz4.c` also be a prerequisite here? `lz4hc.c` includes it directly, so changing only `lz4.c` can leave a stale `lz4hc.o` in an incremental build. Clean CI builds would not catch this.
