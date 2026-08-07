# PR #3853 review draft

Draft review text for manual use. These comments have not been posted to `valkey-io/valkey`.

- Repository: `valkey-io/valkey`
- PR: `#3853` — Streaming Compression support for Replication
- Reviewed head: `1e7389afc1163e59d8ec98c782272d75d8eea90f`
- Intended review event: `REQUEST_CHANGES`
- Revalidated: 2026-08-07

## Top-level review

Thanks @roshkhatri. I took another look at the latest version. The direction looks good, but I think we need to address a few correctness issues before merging.

The runtime config change can restart a full sync even though the decision is supposed to stay frozen for that sync. We are also budgeting replica work using compressed bytes and undercounting the per-replica compression memory. The branch still contains an older version of #3531, and the config, INFO fields and PR description do not match the current implementation.

I also see one failing CI job at this head: `test-centosstream9-tls-module`.

I think the cleanest path is to land #3531 first, rebase this PR on its final version, fix the full-sync toggle behavior, account for decoded work and all codec memory, add the missing regressions, align the public contract and get CI green.

## Inline comment 1

- Path: `src/replication.c`
- Line: `110`
- Side: `RIGHT`

I think this can abort a full sync when `repl-compression` is disabled. The enable path only disconnects online replicas, but the disable path disconnects any replica with a compressor, including one in `REPLICA_STATE_BG_RDB_LOAD`. On the replica side, the config change can also drop `server.primary` or cancel the handshake.

I reproduced this while the primary was in `bg_transfer`: `sync_full` went from 1 to 2 and all 20,000 keys were transferred again. This does not match the documented behavior that the decision stays frozen for an in-progress full sync.

Can we defer the reconnect until the full sync completes and only reconcile online links? We should not change codec state on a live stream.

## Inline comment 2

- Path: `src/rdb.c`
- Line: `3834`
- Side: `RIGHT`

I think this PR has an older and incompatible copy of #3531. This branch contains `401cc3fe7` / `95d44b3e0`, while the current #3531 head is `882bf646d` and is not an ancestor here.

The current #3531 behavior is also different here: it ignores trailing bytes, makes `rdbchecksum` control the LZ4 checksums and propagates codec initialization failures. This branch rejects trailing bytes, always enables codec checksums and asserts on LZ4 context allocation failure.

Can we land #3531 first and rebase this PR on its final version? A merge-tree check currently reports conflicts in `compression_stream.c`, `compression_stream.h` and `server.h`, so I don't think we have a validated combined tree yet.

## Inline comment 3

- Path: `src/compression_repl.c`
- Line: `70`
- Side: `RIGHT`

Are we sure this accounts for all per-replica compression memory? The first frame retains roughly 192 KiB of LZ4 scratch space, a 16,416-byte `LZ4_stream_t` and the codec context, but this only counts `replCompressor` and `out_buf`. We also report zero while `CLIENT_PENDING_IO`, which drops the staged SDS from the accounting.

This feeds `CLIENT LIST` memory, total client memory and output-buffer limit enforcement, so the undercount grows with every compressed replica. Can we track the codec allocations and publish the staging size safely while IO is pending? We should also add an accounting and output-buffer-limit test after compression has started.

## Inline comment 4

- Path: `src/replication.c`
- Line: `3667`
- Side: `RIGHT`

I think we are budgeting compressed bytes here instead of the decoded work. During dual-channel catch-up, `used` is the wire size, but one compressed block can produce much more data for `processInputBuffer()`. The steady-state path has the same issue because it limits socket reads without limiting the total decoded bytes.

For highly compressible input, one event can do a very large amount of work on the replica main thread. Can `replDecompressQueryBuf()` return the produced length so both paths can budget decoded bytes? I think we also need a cumulative logical-byte limit for the steady-state loop and a high-compression-ratio regression.

## Inline comment 5

- Path: `src/config.c`
- Line: `3482`
- Side: `RIGHT`

Can we align the public contract before merging? The PR says `lz4-stream`, a 256 MiB decoder cap and compression off the main thread. The code accepts `lz4`, caps at 16 MiB and can run compression on the main thread when IO threading is inactive, which is the default with `io-threads 1`.

The INFO fields also do not match the description, and I don't see a linked `valkey-doc` PR or a `needs-doc-pr` label. I think the PR description, `valkey.conf`, INFO semantics, tests and docs should describe one behavior.

## Inline comment 6

- Path: `tests/integration/repl-compression.tcl`
- Line: `741`
- Side: `RIGHT`

Can we add coverage for the config changes that happen during an in-progress full sync? The current in-flight test only enables compression during a plaintext dual-channel load. It does not cover disabling compression during a compressed load, or changing the replica-side config during ordinary and dual-channel full syncs.

The tests should verify that `sync_full` does not increase, the current sync finishes using its frozen transport, the reconnect happens after the replica is online and the data is intact. These cases should catch the issue in the config reconciliation path.

## Inline comment 7

- Path: `tests/integration/repl-compression.tcl`
- Line: `822`
- Side: `RIGHT`

Can we also add a rollback test for the deferred reconnect? Both tests here use a successful one-option `CONFIG SET`, so they do not exercise the case where `updateReplCompression()` schedules reconciliation and a later option fails during apply.

With an online compressed replica, I think we should issue a multi-option `CONFIG SET` where the compression option changes and a later option fails, then verify the config is restored and the replica client ID and sync counters do not change. Otherwise a rolled-back command could still cause a visible reconnect.

## Inline comment 8

- Path: `tests/integration/repl-compression.tcl`
- Line: `830`
- Side: `RIGHT`

This looks duplicate of the earlier disable/reconnect test around lines 547-585. Both create one compressed replica, disable compression on the primary and wait for the same plaintext reconnect. Can we keep one and use the saved test time for the missing in-flight toggle cases?
