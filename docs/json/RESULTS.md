# Built-in JSON for Valkey: implementation and benchmark results

Date: 2026-11-15. Status: implemented, measured on Graviton and on x86-64 hardware, tested on x86-64, 32-bit x86 and s390x, reviewed by two independent AI reviewers. Not reviewed by a person.

## 1. Summary

JSON now ships inside `valkey-server` as a built-in module written in C. It replaces the external valkey-json module (C++ on RapidJSON) and stays compatible with it: same 24 commands, same data type name and RDB encoding, same configuration and event names.

Measured against the external module with the same server core on both sides, on Graviton (c7g.16xlarge) and on x86-64 (GitHub-hosted runners, AMD EPYC and Intel Xeon), final code:

- **Memory:** 23% to 49% of the module on eight of nine document shapes, identical on both architectures. The ninth, a large array of short strings, is equal (100%). On eight real-world files it is 6% to 67%.
- **Speed:** faster on all 69 JSON operations on both architectures. The median is 2.69 times on Graviton and 2.30 times on x86-64. The smallest gain is 1.22 times on Graviton (`JSON.GET` of 10,000 short strings) and 1.03 times on x86-64 (`JSON.GET` of a 100,000-record array). With 50 connections, throughput is 1.3 to 2.1 times the module's.
- **Persistence:** `SAVE` takes 1.97 s against 2.47 s on Graviton and 1.71 s against 2.22 s on x86-64, for 500,010 keys. Loading takes 2.02 s against 3.10 s on Graviton.
- **Platforms:** the full Valkey test suite passes with the module built in on x86-64. The JSON tests pass on 32-bit x86 and on s390x (big-endian), which upstream CI builds.
- **Robustness:** the inputs found to crash, hang or desynchronise the external module (section 5) are handled, and two independent reviews led to five further fixes (section 6.1). About three hours each of sanitizer fuzzing and of differential testing (733 million commands, in which the external module crashed 472 times and the built-in never) found one parser bug, now fixed. A 170-minute soak of 341.6 million operations showed no memory growth.
- **Correctness under load and in clusters:** Valkey's whole suite passes with the module under ASan, TLS and valgrind. The JSON tests pass against a cluster, with I/O threads and on musl, and new cluster tests cover slot migration, failover and resharding. The tests reach 99.1% of the module's lines. An RDB file from Valkey 9.0.7 with valkey-json loads with identical data.

The documents are mostly synthetic, and one client or one benchmark tool drove each server. The x86 hosts (`x86 host A`, `x86 host B`, `x86 host C`) were unreachable on port 22 throughout, so x86-64 was measured on GitHub-hosted runners of your fork instead (section 3.10). Those are shared 4-vCPU virtual machines.

## 2. What was built

| Item | Where | Size |
|---|---|---|
| Branch | git worktree `.wt/json`, branch `json-builtin-module`, based on `upstream/unstable` 9e58c3bea | Eight commits, from `5a49c3386` to `2170143fc`, signed off. Not pushed as a branch of its own. The fork's branch `json-builtin-ci` carries them plus five commits that touch only `.github/` (the CI workflow) |
| Module source | `src/modules/json/` | 16,169 lines of C in 12 files |
| Core changes | `src/Makefile`, `src/CMakeLists.txt`, `CMakeLists.txt`, `src/server.c`, `src/config.c`, `src/server.h`, `valkey.conf`, `README.md` | 61 lines added, 2 changed |
| Test suite | `tests/unit/type/json*.tcl`, `tests/integration/json-*.tcl`, one RDB asset | 14 files, 13,059 lines, 1,113 test executions |
| Core test changes | fuzz traffic list, corrupt-payload fuzzer, two introspection lists, a skip rule for servers built without the module | 35 lines |
| Development tests | `src/modules/json/tests/{engine,path,num,cmd}` | 12,811 lines of C and Python |

The module is five source files and their headers. `json_module.c` is the only one that touches the server: entry point, data type callbacks, configuration, statistics, shared API. `json_cmd.c` holds the commands. `json_path.c` compiles and evaluates paths. `json_engine.c` is the storage format, parser, serialiser and key table. `json_num.c` prints and parses numbers.

**Packaging.** The module is compiled into the server and loaded at startup, before the modules in the configuration, the way Lua is loaded in `unstable`. `make BUILD_JSON=no` leaves it out. `json-enabled no` starts the server without it. Configuring an external module named `json` as well stops the server at startup.

**Two module options are set that the external module does not set:** forkless save and atomic slot migration. Because this module is in every server, leaving either unset would have turned that feature off for every user.

## 3. Benchmark

### 3.1 Method

- **Hosts:** `Graviton host A` and `Graviton host B`, both c7g.16xlarge (64-core Graviton3), Amazon Linux 2023, gcc 11.5, jemalloc 5.3. `Graviton host A` is shared with other jobs. `Graviton host B` was idle. The tables below are from `Graviton host A`. Section 3.6 gives the replicate on `Graviton host B`.
- **Servers:** the same Valkey source for both. The reference is built with `BUILD_JSON=no` and loads the external module (the `valkey-json-pr95` checkout at commit 60cfc5c, built `-O3 -DNDEBUG`). The candidate has JSON built in.
- **Isolation:** each server is pinned to two cores, the client to a third. Unix socket only. No TCP listener was opened.
- **Measurement:** server-side microseconds per call from `INFO commandstats`, 200,000 calls for most operations after a warm-up. Three rounds per side, interleaved. Figures are medians of the three.
- **Noise:** across three rounds of one build the median spread per operation is 3.2% and the worst is 12%.
- **Data:** 100,000 documents of each small shape, and one document of each large shape. Generated by `json-m0/gen.py`.

### 3.2 Memory

Bytes per key. For the three record shapes this is the growth of the server's `used_memory` over 100,000 keys, so it includes the key name and the keyspace entry. For the single large documents it is `MEMORY USAGE`.

| Shape | JSON text | External module | Built-in | Ratio |
|---|---:|---:|---:|---:|
| Session record | 307 | 580 | 282 | 49% |
| Profile record | 596 | 1,037 | 506 | 49% |
| Order record | 747 | 1,308 | 541 | 41% |
| Array of 100,000 records | 8,107,476 | 17,835,056 | 5,861,024 | 33% |
| 1,536 doubles at full precision | 32,142 | 61,488 | 14,408 | 23% |
| 1,536 values with six decimals | 14,593 | 24,624 | 7,192 | 29% |
| 100,000 integers | 1,048,299 | 1,835,056 | 458,776 | 25% |
| 10,000 strings of 8 to 13 bytes | 134,802 | 163,888 | 163,864 | 100% |
| One object with 100 members | 1,584 | 4,182 | 451 | 11% |

The built-in stores less than the JSON text itself on eight of the nine shapes.

### 3.3 Time per command

Microseconds per call, server side. The full table of 76 rows is in `json-m0/bench/results/report-c3.md`.

| Operation | External module | Built-in | Speed-up |
|---|---:|---:|---:|
| `JSON.SET` whole session record | 4.05 | 1.76 | 2.30× |
| `JSON.SET` whole profile record | 6.27 | 2.58 | 2.43× |
| `JSON.SET` whole order record | 8.96 | 3.60 | 2.49× |
| `JSON.GET` whole profile record | 2.52 | 0.84 | 3.00× |
| `JSON.GET $.address.city` | 1.77 | 0.52 | 3.40× |
| `JSON.GET` three paths | 3.63 | 0.88 | 4.12× |
| `JSON.TYPE $.age` | 1.15 | 0.45 | 2.56× |
| `JSON.MGET` 10 keys | 10.97 | 4.40 | 2.49× |
| `JSON.GET $..price` | 12.73 | 1.48 | 8.60× |
| `JSON.GET $.items[?(@.qty>4)].sku` | 3.97 | 0.99 | 4.01× |
| `JSON.NUMINCRBY $.age 1` | 1.69 | 0.64 | 2.64× |
| `JSON.NUMINCRBY $.score 0.25` | 2.92 | 0.77 | 3.79× |
| `JSON.SET $.age` | 1.57 | 0.62 | 2.53× |
| `JSON.SET $.name`, length changes | 1.55 | 0.85 | 1.82× |
| `JSON.STRAPPEND $.name` | 1.42 | 0.90 | 1.58× |
| `JSON.ARRAPPEND $.tags` | 1.73 | 0.95 | 1.82× |
| `JSON.ARRINSERT $.tags 0` | 1.53 | 0.97 | 1.58× |
| `JSON.MERGE $.prefs` | 3.53 | 1.42 | 2.49× |
| `JSON.SET $.items[*].price` | 8.02 | 1.40 | 5.73× |
| 100,000 records: `JSON.SET` whole | 90,780 | 43,623 | 2.08× |
| 100,000 records: `JSON.GET` whole | 14,428 | 13,188 | 1.09× |
| 100,000 records: `JSON.GET $[n].name` | 1.15 | 0.57 | 2.02× |
| 100,000 records: `JSON.GET $..price` | 153,051 | 17,538 | 8.73× |
| 100,000 records: filter on price | 17,628 | 3,302 | 5.34× |
| 100,000 records: `JSON.DEL` key | 28,020 | 18.65 | 1,502× |
| Full-precision doubles: `JSON.SET` | 284.32 | 94.43 | 3.01× |
| Full-precision doubles: `JSON.GET` | 67.48 | 26.30 | 2.57× |
| Six-decimal values: `JSON.GET` | 32.00 | 12.90 | 2.48× |
| 10,000 short strings: `JSON.SET` | 311.45 | 182.81 | 1.70× |
| 10,000 short strings: `JSON.GET` | 83.96 | 68.41 | 1.23× |

Over the 69 JSON operations: median 2.49 times, geometric mean 2.63 times with any single ratio capped at 10, slowest 1.09 times, none slower than the module.

Three notes on reading the table.

- The 1,502 times on deleting the large document is a change of behaviour, not of speed. The built-in reports how much work a free is, so the server frees a large document on its background thread. The module frees it on the main thread and blocks every client for 28 ms.
- A single-path `JSON.GET` costs about the same as `HGET` on the same data and host (0.52 µs against 0.37 to 0.46 µs), and `JSON.NUMINCRBY` costs about the same as `HINCRBY` (0.64 against 0.65 to 0.76 µs).
- Core commands (`GET`, `SET`, `HGET`, `HSET`, `HINCRBY`, `STRLEN`) measured on both servers are equal within run-to-run variation. The hash commands are bimodal on this host on both sides.

### 3.4 Save, load, free

500,010 keys, about 212 MB of RDB.

| | External module | Built-in |
|---|---:|---:|
| `SAVE`, seconds | 2.469 | 1.973 |
| Load, seconds | 3.097 | 2.020 |
| `FLUSHALL SYNC`, seconds | 0.729 | 0.195 |

### 3.5 Where the remaining time goes

A profile of `JSON.TYPE` on a random key (0.45 µs) shows the JSON functions at 17% of the samples. The rest is the server's command handling and two cache misses: the document's first bytes, and the 16-byte wrapper the module API allocates for every module value. The wrapper is the main cost of being a module and not a native type. Removing it is a change to the server core that would benefit every module.

### 3.6 Replicate on a second host

The same three interleaved rounds on `Graviton host B`, an idle host of the same type, with the build from before the review fixes.

| | `Graviton host A` (shared) | `Graviton host B` (idle) |
|---|---:|---:|
| Memory, all nine shapes | as in 3.2 | identical byte counts |
| JSON operations faster than the module | 69 of 69 | 69 of 69 |
| Median speed-up | 2.49× | 2.64× |
| Smallest speed-up | 1.09× | 1.14× |
| `JSON.GET $.address.city`, µs | 0.52 | 0.39 |
| `HGET`, µs, module server and built-in server | 0.37 and 0.45 | 0.36 and 0.36 |
| `SAVE`, seconds, module and built-in | 2.469 and 1.973 | 2.464 and 1.941 |

On the idle host the core commands cost the same on both servers, which confirms that the difference seen on the shared host was noise. Raw results: `json-m0/bench/results/h2-*.json` and `report-h2.md`.

**After the review fixes.** The final code, with the fixes of section 6.1, was measured on `Graviton host B` in three more rounds against the same module baseline: faster on 69 of 69, median 2.60 times, smallest gain 1.13 times, memory unchanged. Against the build before the fixes on the same host, the median cost of the fixes is 1.1%. The largest is 9% on `JSON.MSET` (2.16 to 2.36 µs), from validating member names. Raw results: `h3-cand-*.json` and `report-h3.md`.

### 3.7 Real documents

Eight JSON files from the external module's test data, each stored as one key, on `Graviton host B` with the final code. Memory is `MEMORY USAGE`. Times are server-side microseconds per call. Every document read back equal to its source on both servers.

| File | JSON text, bytes | Module memory | Built-in memory | Ratio | `JSON.SET` µs, module and built-in | `JSON.GET` µs, module and built-in |
|---|---:|---:|---:|---:|---:|---:|
| `twitter.json` | 631,514 | 560,616 | 263,336 | 47% | 2,949 and 1,307 | 483 and 223 |
| `apache_builds.json` | 127,275 | 152,368 | 91,720 | 60% | 628 and 257 | 65.4 and 47.3 |
| `github_events.json` | 65,132 | 70,976 | 47,800 | 67% | 273 and 103 | 45.6 and 23.3 |
| `truenull.json` | 12,000 | 32,816 | 2,072 | 6% | 37.2 and 14.0 | 22.3 and 4.7 |
| `webxml.json` | 3,468 | 3,160 | 1,304 | 41% | 19.0 and 7.7 | 2.82 and 1.33 |
| `store.json` | 1,895 | 1,912 | 664 | 35% | 11.2 and 4.9 | 2.05 and 1.04 |
| `wikipedia.json` | 432 | 528 | 184 | 35% | 3.96 and 1.87 | 1.00 and 0.45 |
| `wikipedia_compact.json` | 309 | 528 | 184 | 35% | 3.73 and 1.80 | 1.00 and 0.45 |

The two files at 60% and 67% are dominated by long strings (URLs and descriptions), which both implementations store as they are. `JSON.SET` is 2.1 to 2.7 times as fast on every file, and `JSON.GET` 1.4 to 4.7 times.

### 3.8 Many clients

`valkey-benchmark` with 50 connections, pipeline depth 16 and 8 client threads over a unix socket, 100,000 profile documents, on `Graviton host B`. The server has two cores and the client twenty. Best of two runs, requests per second. No command returned an error.

| Command | External module | Built-in | Ratio |
|---|---:|---:|---:|
| `JSON.SET` whole profile | 147,827 | 292,084 | 1.98× |
| `JSON.GET` whole profile | 332,594 | 630,517 | 1.90× |
| `JSON.GET $.address.city` | 399,255 | 856,164 | 2.14× |
| `JSON.NUMINCRBY $.age 1` | 399,308 | 748,503 | 1.87× |
| `JSON.SET $.name` | 399,361 | 665,484 | 1.67× |
| `JSON.ARRAPPEND $.tags` | 374,345 | 544,267 | 1.45× |

Raw results: `json-m0/bench/results/real-*.json`.

### 3.9 x86-64 under emulation, before hardware was available

No x86 host was reachable, so x86-64 was checked on the Mac with the server compiled for x86-64 and run under Rosetta 2. This exercises the x86-64 code the compiler generates. It is not a measurement of x86 hardware.

**Correctness.** The committed tree builds for x86-64 with no warnings and passes: 1,171 Tcl tests (the JSON files, minus the cases that need the external module, plus the two core test files the change touches), the 3,354-step directed comparison against the real module, the 36,205 path cases, and the engine and number suites including their ASan and UBSan builds (309,210 change steps).

**Relative cost.** Both servers and the external module built for x86-64, two interleaved rounds at one fifth of the data size, with no core pinning on a busy laptop. The built-in is faster on 69 of 69 operations, median 2.71 times, smallest gain 1.02 times (`JSON.SET` of 100,000 integers). Memory under the macOS allocator: 45%, 46% and 38% on the three record shapes, 34% on the record array, 17% and 27% on the two number arrays, 27% on integers, 100% on short strings. Raw results: `json-m0/bench/results/x86r-*.json` and `report-x86-rosetta.md`.

### 3.10 x86-64 hardware, GitHub Actions

The workflow `.github/workflows/json-builtin.yml` on the fork's branch `json-builtin-ci` runs the same benchmark on a GitHub-hosted runner: 4 vCPUs, the server pinned to two, the client to a third, three interleaved rounds per side, the external module built `-O3 -march=nehalem` at the same commit as the Graviton baseline. The runner's processor varies between runs. Final code, run 43694898886, Intel Xeon 6975P-C:

| | External module | Built-in | Ratio |
|---|---:|---:|---:|
| Memory, all nine shapes | | | identical to Graviton (section 3.2) |
| JSON operations faster than the module | | | 69 of 69, median 2.30×, smallest 1.03× |
| `JSON.SET` whole profile record, µs | 4.41 | 1.89 | 2.33× |
| `JSON.GET $.address.city`, µs | 1.26 | 0.45 | 2.80× |
| `JSON.GET` of the 100,000-record array, µs | 10,910 | 10,552 | 1.03× |
| `SAVE`, 500,010 keys, seconds | 2.222 | 1.709 | 1.30× |
| `twitter.json` (632 KB): `JSON.SET` and `JSON.GET`, µs | 1,911 and 273 | 790 and 138 | 2.42× and 1.98× |
| 50 connections, single-path `JSON.GET`, requests/s | 520,291 | 919,681 | 1.77× |
| 50 connections, `JSON.ARRAPPEND`, requests/s | 497,842 | 663,716 | 1.33× |

Before the serialiser change of `2c17a3a01`, two runs on AMD EPYC 9V74 runners had the same memory, a median of 2.29 times, and the whole-document read of the record array at 0.97 times, the only operation slower than the module on any platform. A profile on the runner showed the serialiser's member loop and an out-of-line copy helper. The change made that read 10% faster on Graviton and turned it into a gain on x86-64. The two runs were on different processors, so the x86 comparison of before and after is indicative.

Raw results: `json-m0/bench/results/gha-x86/`, `gha-x86-2/` (before) and `gha-x86-final/` (after).

## 4. What changed from the design

`VALKEY_JSON_DESIGN.md` and `VALKEY_JSON_NARRATIVE.md` now describe the system as built. These are the differences from the design as it stood before implementation, all driven by measurements on the host.

| Change | Why | Effect |
|---|---|---|
| Decimals of up to 18 significant digits are stored as an integer and a scale, in scalars and in typed arrays | Printing doubles was 86% of reading an array of decimals, and the module beat us by 2 times there | Six-decimal arrays went from 58% to 29% of the module's memory and from 0.51 to 2.48 times its read speed. Full-precision arrays went from 0.95 to 2.57 times. The per-array "fixed format" in the design is gone |
| Arrays of strings of up to 15 bytes use 16-byte inline elements | One allocation per string made big string arrays 148% of the module's memory and half its write speed | 100% of the module's memory, 1.70 times its write speed |
| Arrays of only `true`, `false` and `null` use one byte per element | 40 lines on the existing mechanism | An eighth of the previous size for that shape |
| Compiled paths are cached, 256 entries | Compiling the path was a quarter of a single-path read | A repeated path costs 14 to 20 ns to compile, evaluate and release, with no allocation |
| A new shortest-digits number printer and a correctly rounded parser, written here | Valkey's `fpconv` is not always shortest | 26 to 35 ns per number. Checked against Python on 33 million doubles |

## 5. Compatibility

**Identical:** `TYPE` replies `ReJSON-RL`. `MODULE LIST` shows `json`. RDB and `DUMP` payloads load in both directions, checked both ways with the real module. Command names, flags, key specifications, ACL category, keyspace events, replication stream, `INFO` section and field names, configuration names, the `JSON_GetValue` shared API.

**The ten listed changes (B1 to B10 in the design)** are implemented as designed. Eight of the module's own 127 integration tests fail because of them: four on computed number formatting (B3), one on nested matches (B4), one on member names containing `~` or `/` (B5), two on removed `JSON.DEBUG` subcommands (B8).

**Further differences found by testing.** Each is a case where the module's behaviour is an accident.

| Case | External module | Built-in |
|---|---|---|
| JSON text with an embedded or trailing NUL | Stores the text before the NUL | Error |
| Member name containing NUL | Cannot be deleted | Works |
| `JSON.RESP` of a number | Prints the double with 17 digits: `16.424925` becomes `16.424925000000002` | The shortest digits of the value: `16.424925`. A stored `19.90` replies `19.9` |
| `-5` multiplied by `0` | `-0` | `0` |
| Slice step 2147483648 or 4294967296 | Never returns | First element |
| Slice step with bit 31 set, index at or above 2^64 | Wraps around | First element, out of range |
| `JSON.RESP` with a legacy path matching several values | One reply per match, which breaks the protocol | First match |
| `JSON.SET k $ 1.8e308` then `JSON.NUMINCRBY` | Server aborts | Rejected at `JSON.SET` |
| `[5e-324]` then `JSON.RESP` | Server aborts or hangs | Works |
| `JSON.DEL` of several values in a document with a `~` in a name | Server crashes | Works |
| `json_total_memory_bytes` right after deleting a large document | Exact at once | Settles within milliseconds, because the free runs in the background |
| `JSON.DEBUG MAX-SIZE-KEY` and `MAX-DEPTH-KEY` | Reply the key name as a simple string. A key name containing CR LF injects extra replies | Reply it as a bulk string |
| A path with a repeated union member chained many times, such as `['a','a']` 22 times | Work doubles with every step: 1.07 s and a 209 MB reply at 22 steps | Small repeats reply as before. Beyond a work bound the command fails with a `LIMIT` error in 1.6 ms |
| `JSON.GET` with `INDENT`, `NEWLINE` or `SPACE` whose reply would exceed 512 MB | Builds it | `LIMIT` error before building it |
| A new member name that is not valid UTF-8, through a path | Accepted | Rejected for an ordinary client |
| A document nested deeper than `json.max-path-limit` on load (B9) | Rejected | Accepted up to 1,024 levels or the limit, whichever is larger. The same bound applies to commands from a primary or an AOF file |
| Recursive descent on a document more than 4,096 levels deep | No limit. The process dies at 20,000 levels | `LIMIT` error |
| A quoted name after a comma in a union, or a quoted value on the left of a filter comparison, that starts with spaces: `$['x',' a']`, `$[?(' a'==@.n)]` | Drops the leading spaces: looks up `a`, compares with `"a"` | Keeps them |
| A number on the left of a filter comparison with a space inside: `$[?(5 0!=@.n)]` | Reads it as 5 and ignores the 0 | `Invalid JSON path` |

**The plain C symbols** that `libjson.so` exports (`is_json_key`, `is_json_key2`, `get_json_value_type`, `get_json_value`, `get_json_values_and_types`) are exported by the server with the same signatures and ownership rules. A test module calling them gives identical results on both implementations in 67 cases.

**Mixed-version replication** was run in both directions on loopback. Full sync gives identical documents and digests each way. Over a stream of 690 writes, 25 keys diverge with a module primary and 26 with a built-in primary. Every one is a listed change (B3, B4, B5, B6) or a module defect. Text that the module accepts and the built-in rejects from clients (invalid UTF-8, out-of-range numbers, NUL bytes) causes no divergence on a built-in replica. AOF files written by one and loaded by the other agree after a rewrite, except one 129-level document that the module refuses.

**Upgrade from Valkey 9.** Run on Linux arm64 against Valkey 9.0.7 with valkey-json 1.1.0 and with 1.0.3, using 101,698 keys of every shape plus edge cases. An RDB file written by 9.0.7 loads into the built-in with all 101,698 keys identical in value, type and TTL, before and after a reload. An AOF file (with and without an RDB preamble) and a replication stream of 21,197 commands from 9.0.7 give the same data, except values changed by computed number formatting (B3). Over 47,778 probe commands on the upgraded data, the replies differ in 2,864, all B3 except one: a string containing a NUL, which the module had cut short. Downgrade by file is blocked by the core, not by JSON: the built-in server writes RDB version 81 and 9.0.7 reads at most 80. With `rdb-version-check relaxed` on 9.0.7 the RDB loads with identical data. A rolling upgrade (replicas first) works. During the upgrade, a primary on the built-in can replicate `JSON.MERGE` to a replica on valkey-json 1.0.3, which lacks the command. That is version skew of the module, present today between 1.0.3 and 1.1.0.

**Platforms.** The module runs on 64-bit and 32-bit targets of either byte order. On a 32-bit target a key's value points to an 8-byte heap cell holding the document word. On 32-bit x86 the module is built with SSE2 arithmetic, because x87 registers made one number comparison differ from every other target. Porting found two real bugs: the key hash collided on big-endian for names differing only in their last bytes, and some sizes could wrap a 32-bit counter. The JSON tests pass on 32-bit x86 (GitHub runner, real hardware, 1,301 tests including active defrag) and on s390x (GitHub runner under qemu, all JSON tests except the timing-based active defrag ones).

**valkey-search.** valkey-search itself was not run: it is not built on this machine. Its integration surface was read from its source and exercised by a test module that does the same things the same way: finds a module named `json` through `MODULE LIST`, takes `JSON_GetValue` from the shared API with the same signature, falls back to `JSON.GET`, treats a key of module type as JSON, and subscribes to the same keyspace events. Loaded into both implementations, it gave 0 differences over 157 commands and 58 keyspace events. valkey-search compares no version number. The built-in reports module version 20000.

## 6. Testing

| Suite | Size | Result |
|---|---|---|
| Engine against an independent Python model | 1,192,436 comparisons, 357,135 change steps, five cap settings | 0 failures, also under ASan and UBSan. The quick form of the engine, number and path suites also passes with gcc on Linux, with no warnings |
| Engine, deliberately planted bugs | 34 across four phases | 33 caught. The one miss led to a new check, and that bug is caught now |
| Number printer and parser against Python | 33.3 million prints, 16.0 million parses, 6.0 million mantissa and exponent pairs | 0 failures |
| Path evaluator against the real module | 36,205 document and path pairs | 0 failures |
| Commands, directed, against the real module | 3,354 steps, RESP2 and RESP3, replies with wire types, keyspace events, replication stream | 0 failures |
| Commands, random differential, against the real module | 12 seeds of 50,000 commands | No crash, no desync. 99 to 154 differences per seed outside B1 to B10, all in the classes of section 5 |
| Persistence against the real module | 21 checks: reload, `DUMP` and `RESTORE` both ways, `COPY`, RDB both ways | The built-in passes all |
| Replication and AOF between the two implementations | Both directions, 690 writes, AOF both ways | 0 unclassified divergences (section 5) |
| Plain C API through a test module | 67 calls on both implementations | Identical |
| The interfaces valkey-search uses, through a test module | 157 commands and 58 keyspace events on both implementations | Identical |
| The module's own pytest suite | 127 cases | 118 pass. Eight fail by design, one on the background free |
| New Tcl suite | 14 files, 1,113 executions | All pass on macOS (Tcl 9.0 and 8.6). On Linux (Tcl 8.6) 1,107 pass and none fail, inside the full run below |
| Active defrag of JSON documents | 4 tests, Linux with jemalloc | All pass |
| Final code on GitHub-hosted runners (`2c17a3a01`) | x86-64: engine, number, path and command suites against the external module, replication, Valkey's full test suite. 32-bit x86: JSON tests and touched core files. s390x: path suite and JSON tests | All four jobs pass |
| Final code, engine suite on Linux arm64 (`Graviton host A`, gcc, ASan and UBSan) | Full form | 0 failures |
| 32-bit and big-endian port, engine suites in containers | i386 and s390x, quick form with UBSan, plus a byte-layout corpus of 4,998 documents | 0 failures. Three bugs planted for the port were each caught on the affected target |
| Valkey's full test suite with the module built in, Linux, final code | 200 test files, 7,445 tests | All pass. An earlier run had one failure, "CLIENT KILL close the client connection during bgsave - forkless", which also fails 3 of 3 on a server built without the module: the test waits 10 s for a save that sleeps 10 s |
| Valkey's full test suite with the module built in, macOS | 200 test files, 7,335 tests, run before the review fixes | 7,332 pass. Three fail: one cluster `del-node` test that fails 2 of 2 on pristine upstream at the same commit, and two replication-throttle tests that pass when rerun alone |
| The JSON tests on a server built without the module | 14 files | Every JSON block is skipped, 0 failures |
| Corrupt-payload fuzzer with JSON documents | Default duration | Pass |
| Long fuzzing, Linux arm64 (`Graviton host A`), gcc with ASan and UBSan, about 3 hours | Engine: 9 seeds, 7.0 million iterations. `RESTORE` of mutated payloads: 2.86 million. Paths: 87.5 million commands against an ASan server and a UBSan server side by side | 0 sanitizer reports, 0 mismatches, 0 leaked keys at shutdown |
| Commands, random differential against the real module, 3 hours, 24 workers (`Graviton host B`) | 733 million commands, half RESP2 and half RESP3, and 251 persistence runs | The built-in never crashed. The module crashed 472 times. Three new kinds of difference: two module defects and one parser bug in the built-in, now fixed (section 5, the last two rows) |
| Soak, release build with jemalloc, 170 minutes | 341.6 million operations at 34,000 per second, 42 `BGSAVE` (24 fork, 18 forkless), 5 `DEBUG RELOAD` | 0 unexpected errors, no crash. No memory trend after warm-up: `used_memory` −0.13% per hour, document memory +0.11% per hour. p99 about 10 µs. The document count equalled `DBSIZE` at every quiet point |
| Cluster tests (new): slot migration, keyspace, failover, resharding | 23 tests, three runs | All pass |
| The JSON test files against a cluster | 15 files | 994 pass, 0 fail, after test fixes: keys used together now share a hash tag |
| RDB encoding 0, the external module's oldest format (new test file) | Hand-built payloads of every node type, nesting at and beyond the load bound, over 100 malformed payloads | All valid payloads load, all malformed ones are refused, no crash |
| Full suite under ASan, module instrumented (GitHub x86-64) | Valkey's whole suite with the module built in, gcc and clang | gcc 7,442 and clang 7,437 pass, 0 fail, no sanitizer report |
| Full suite under UBSan, module instrumented | Whole suite, gcc and clang | gcc 7,452 and clang 7,444 pass, 0 fail, no UBSan report |
| Full suite, final code (`2170143fc`) | Whole suite, x86-64 | 7,495 pass, 0 fail. Earlier runs each had one upstream cluster test fail that is known to be flaky upstream (valkey-io/valkey#5215, and `failover2.tcl` "drop_nack") |
| Cluster suite and the JSON files on a cluster, final code | `tests/unit/cluster` with the module built in, and 15 JSON files against a single-node cluster | 1,020 and 994 pass, 0 fail |
| Full suite with TLS | Whole suite | 7,408 pass, 0 fail |
| valgrind | Upstream's unit, type, integration and cluster shards plus three JSON shards | All pass, no valgrind error |
| musl (Alpine) and I/O threads | JSON files | 1,117 pass on each. The I/O-thread job's core subset had one upstream timeout, `pause.tcl` "Busy script ending…", which also fails without JSON |
| Line coverage of the module, from all test suites | gcov | 99.1% of lines (8,162 of 8,235), 99.4% of functions (541 of 544) |
| Upgrade from Valkey 9.0.7 with valkey-json 1.1.0 and 1.0.3 | RDB, AOF with and without preamble, replication stream, 101,698 keys | Identical data except B3 (section 5) |

### 6.1 Independent review

Two AI reviewers that had not seen the code before read it adversarially and attacked it. Neither is a substitute for review by a person.

**Engine and number code.** No confirmed defect. Covered: fuzzing of parse, serialise, change, copy, defrag and free under ASan and UBSan, a thread-sanitizer run of the key table with three lazy-free threads and a save thread (2.1 million iterations), and nesting of 500,000 levels.

**Commands, paths and glue.** No memory-safety fault, leak or protocol desync in about 6 million stress commands under ASan. Five confirmed defects, all fixed, each with a test:

| Finding | Fix |
|---|---|
| A path with repeated union members forks exponentially: 1.07 s and 209 MB for one `JSON.GET`. The module has the same defect | A work bound relative to the document. The same query fails in 1.6 ms |
| AOF rewrite lost a document nested deeper than `json.max-path-limit` | Commands from a primary or an AOF file skip the client limits |
| A crafted `RESTORE` payload of 20,000 levels made one query use 1.6 GB | Load accepts at most 1,024 levels or the limit, whichever is larger. Recursive descent stops 4,096 levels below its start. Nested matches share one locator |
| `JSON.DEBUG MAX-SIZE-KEY` let a key name inject protocol replies. The module has the same defect | Bulk string reply |
| Invalid UTF-8 could enter through a new member name in a path | Validated for ordinary clients |

The review also found that the module refused to compile on 32-bit and big-endian targets, which upstream CI builds (section 5, Platforms), and the Tcl tests for these fixes found one more: a replicated `JSON.ARRAPPEND` could nest a document one level past the load bound.

## 7. What is not done and not known

- **x86 hosts of your own.** The x86 hosts were unreachable. x86-64 was measured on shared GitHub-hosted 4-vCPU virtual machines (section 3.10), whose processor changes between runs.
- **Production data.** Eight real-world files were measured (section 3.7) and the rest is synthetic. Documents dominated by long strings save less: 60% to 67% of the module's memory.
- **Scale.** The many-client benchmark used one server on two cores with no I/O threads. Cluster mode, I/O threads and forkless save were tested for correctness (section 6), not measured for speed.
- **Review by a person.** None. The code and tests were written by AI agents working to written interfaces, checked by tests against the real module and by two AI reviewers.
- **valkey-search** itself was not run against the built-in. A test module that uses the same interfaces the same way found no difference (section 5).
- **Big-endian hardware.** s390x ran under qemu emulation only, so active defrag timing was not exercised there.
- **Long fuzzing.** Done for about 3 hours per fuzzer and differential run, and a 170-minute soak (section 6). Not days.
- **Encoding-0 doubles.** A document saved in the external module's oldest RDB format (encoding 0) stores doubles as binary. The loader turns each into 17 significant digits of text, as the module does, so 3.0 loads as the integer 3 and 0.1 as `0.10000000000000001`.
- **Format check.** Done: every touched C file is clean under clang-format 18.1.8, the version CI pins, and under 19 to 22.
- **Development tests** live under `src/modules/json/tests/` and are not wired into `make test`. Upstream would want the engine tests in the unit test framework.
- **One statistic is slightly off:** a member name used by a recently cached path stays counted in `json_total_memory_bytes` (16 bytes per name) after its documents are gone.
- **Nested recursive descents** such as `$..*..*` are bounded in memory but not tightly in time: 396 ms on a document 2,000 levels deep with 8,000 values.
- **Documentation.** The `valkey-doc` changes are committed on the local branch `json-builtin` of `~/workspace/valkey-doc` (commits `a7a40a1`, `8d49e06` and `a56783c`, 16 files: the JSON topic page, ten command pages, the data types and quick start pages). The link check passes. The spell checker was not run because it needs a download. Not pushed.
- **Not published upstream.** The commits are on the fork's CI branch `json-builtin-ci` (with one CI commit on top), not on a branch of their own and not in a pull request. The pull-request text and the exact commands are in `json-m0/docs/UPSTREAM_PR.md` and `UPSTREAM_PR_BODY.md`.

## 8. How to reproduce

Build and test:

```
cd .wt/json
make -j
./runtest --single unit/type/json --single integration/json-persistence
```

Benchmark on the host (everything is under `/dev/shm/json-bench/`, which is memory-backed and disappears on reboot):

```
json-m0/bench/sync-cand.sh
python3 bench.py --server ../valkey-cand/src/valkey-server --name builtin --out cand.json --server-cpu 62,63 --client-cpu 60
python3 bench.py --server ../valkey-ref/src/valkey-server --module /dev/shm/json-bench/valkey-json/libjson.so --name module --out module.json --server-cpu 62,63 --client-cpu 60
python3 report.py module.json cand.json
```

Raw results: `json-m0/bench/results/c3-*.json`. Compatibility tools: `json-m0/compat/README.md`.
