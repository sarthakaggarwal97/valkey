# JSON for Valkey, rewritten in C: six-page narrative

Document type: narrative reporting a result and asking for three decisions. Date: 2026-11-15.
Appendix A, the technical design, is `VALKEY_JSON_DESIGN.md`. The
results write-up is `VALKEY_JSON_BUILTIN_RESULTS.md`. The raw measurements are in
`json-m0/bench/results/`.

## 1. Purpose

The earlier version of this document asked for two decisions and a measurement milestone. This
version reports that the engine is built, measured inside the server and reviewed by two AI
reviewers, and asks for three decisions on what comes next.

**Result.** JSON now runs inside `valkey-server` as a built-in module written in C. It replaces
the external valkey-json module and stays compatible with it: the same 24 commands, type name,
module name, file formats and search interface. Measured on Graviton and on x86-64 hardware
against the external module, with the same server core on both sides, it uses 11% to 49% of
the module's memory on eight of nine document shapes and equals it on the ninth. The final code
is faster on all 69 JSON operations measured on both architectures: median 2.69 times on
Graviton and 2.30 times on x86-64, minimum 1.22 and 1.03 times. On eight real-world files it uses 6% to 67% of the module's memory, and with 50 connections it serves
1.45 to 2.14 times the requests per second. Ten listed behaviours change, and testing and
review found sixteen smaller differences.

**The two earlier decisions.**

| Decision | Outcome |
|---|---|
| Ship as a built-in module, not a native core type | Directed by the owner on 2026-11-14, and built that way |
| Keep number text exact, not normalised | Built as recommended. It was not separately confirmed. [Input needed: confirmation and owner] |

**Decisions needed now.**

| # | Decision | Recommendation | Owner | Needed by |
|---|---|---|---|---|
| 1 | Accept the ten behaviour changes and the sixteen further differences (Appendix A, sections 2.3 and 2.5) | Yes | [Input needed] | Before the upstream proposal |
| 2 | Fund the remaining validation: review by people, valkey-search itself, longer fuzzing | Yes | [Input needed] | [Input needed] |
| 3 | Open the upstream proposals in valkey-io when that validation holds | Yes | [Input needed] | [Input needed] |

## 2. The customer and the problem

The customer is a Valkey operator who stores JSON documents and pays for the memory they
occupy. Three shapes cover the workloads this design targets: many small records of 200 bytes
to 2 KB such as sessions and profiles, one key holding a large array of records such as a
catalog, and a vector embedding of 1,000 to 2,000 numbers indexed by valkey-search.

Today these customers load the valkey-json module. Its command set is the contract this work
keeps. We measured the real module on the benchmark
host with 100,000 documents of each small shape. Memory is bytes per key, including the key
name.

| Shape | Text, bytes | Module memory, bytes | Ratio to text |
|---|---:|---:|---:|
| Session record | 307 | 580 | 1.9× |
| Profile record | 596 | 1,037 | 1.7× |
| Order record | 747 | 1,308 | 1.8× |
| Array of 100,000 records | 8,107,476 | 17,835,056 | 2.2× |
| 1,536 numbers at full precision | 32,142 | 61,488 | 1.9× |

The module builds every document from 16-byte nodes with a separate allocation for each
container and each string longer than 13 bytes. It keeps every non-integer number as text, so
each number of the full-precision embedding costs 40 bytes.

Memory is the first problem. Main-thread stalls are the second. The module frees a document
on the main thread: deleting the 100,000-record key blocks every client for 28 ms.
It defragments a document by copying all of it in one call, and it skips any document above
64 MB. Its maintainers reported on 2024-12-11 that this copy receives the memory it just
freed, which defeats the purpose (valkey-io/valkey-json issue 26). The third problem is operational: JSON is 8,000 lines of C++ on top of a parsing
library, built, versioned and loaded separately from the server.

**What we do not have.** This document contains no customer usage data. We do not know how many
deployments load the module, what share of their memory is JSON, or which shape dominates.
[Input needed: number of customers or clusters using JSON, their JSON memory footprint, and any
customer-reported memory or latency complaints.]

## 3. Tenets

In priority order.

1. **Break as little as possible.** A client, a tool or a data file that works with the
   module today works unchanged. A change is acceptable only when it is good for the
   long term, and every change is listed.
2. **Measure, then decide.** A design choice that measurement settles is settled that way.
3. **Memory first, then speed.** Where the two conflict, the design takes the memory saving and
   holds speed to "no slower than the module".
4. **One mechanism per job.** Simple means fewer moving parts, not fewer wins. An optimisation
   that serves a stated target stays. Machinery that serves no target stays out.

## 4. What was built

**The engine.** A small document, or a small part of a large one, is stored as one contiguous
block. Object keys are replaced by short
integer IDs shared across all documents, so the field name `email` is stored once for a million
records. A container that outgrows the packed form becomes a tree of 8-byte slots whose small
children stay packed. Arrays of integers, decimals,
doubles, short strings and literals are stored as plain arrays of the narrowest element. The
parser writes to a reusable scratch area and allocates the result at its exact size. No walk
recurses on document depth. A write moves bytes inside the block it touches.

**Numbers.** `19.90` still comes back as `19.90`. A number in plain notation with up to 18
significant digits is held as an integer and a scale. Its digits take 1 to 8 bytes and print
back exactly, without a floating-point conversion. A new shortest-digits printer and a
correctly rounded parser, written for this module, cost 26 to 35 ns per number.

**The packaging.** The engine is a module compiled into the server and loaded at startup, as
Lua is. To every client and tool it is still the `json` module: `TYPE` replies
`ReJSON-RL`, `MODULE LIST` shows it, and snapshot files and `DUMP` payloads load in both
directions with the external module. Operators no longer need a `loadmodule` line. The build
option `BUILD_JSON=no` leaves it out, and the startup option `json-enabled no` turns it off so
that an operator keeps the external module. It builds on 64-bit and 32-bit targets of either
byte order, so every build upstream CI makes has JSON.

**Measured results.** Bytes per key, in the server, against the external module.

| Workload | Module, bytes | Built-in, bytes | Ratio | Target |
|---|---:|---:|---:|---|
| Session record | 580 | 282 | 49% | At most 50% |
| Profile record | 1,037 | 506 | 49% | At most 50% |
| Order record | 1,308 | 541 | 41% | At most 50% |
| Array of 100,000 records | 17,835,056 | 5,861,024 | 33% | At most 60% |
| 1,536 numbers at full precision | 61,488 | 14,408 | 23% | At most 25% |
| 1,536 numbers with six decimals | 24,624 | 7,192 | 29% | At most 60% |
| 100,000 integers | 1,835,056 | 458,776 | 25% | At most 30% |
| 10,000 strings of 8 to 13 bytes | 163,888 | 163,864 | 100% | None set |

Server-side time per command on the first Graviton host, before the final change, in
microseconds.

| Operation | Module | Built-in | Speed-up |
|---|---:|---:|---:|
| `JSON.SET` whole profile record | 6.27 | 2.58 | 2.43× |
| `JSON.GET` whole profile record | 2.52 | 0.84 | 3.00× |
| `JSON.GET $.address.city` | 1.77 | 0.52 | 3.40× |
| 100,000 records: `JSON.GET` whole | 14,428 | 13,188 | 1.09× |
| Full-precision numbers: `JSON.GET` | 67.48 | 26.30 | 2.57× |

No operation is slower than the module. A single-path `JSON.GET` takes 0.52 µs where `HGET` on
the same data takes 0.37 to 0.46 µs. For 500,010 keys, `SAVE` takes 1.97 s against 2.47 s and
loading takes 2.02 s against 3.10 s. Deleting the 100,000-record key takes 18.65 µs against
28,020 µs, because a large document is now freed in the background.

One shape is at memory parity: a large array of short strings costs 16.4 bytes per element in
both implementations. The one operation that was slower in the prototype, reading back an
array of floating-point numbers, is now 2.48 to 2.57 times the module's speed.

## 5. What changes for customers

Ten behaviours change. Six fix a defect and four follow from the new engine and packaging.
These five are the ones an ordinary client or operator sees.

1. **Invalid text is rejected.** JSON containing bytes that are not valid UTF-8 is refused for
   new writes. Data already stored is kept and still loads. Today the module stores such input
   and then returns invalid JSON.
2. **Arithmetic results are cleaner.** `1.1 + 1` replies `2.1`, where the module replies
   `2.1000000000000001`. `1.5 + 1.5` replies `3.0`, where the module replies `3`.
3. **A key name containing `~` or `/` is stored as written.** Today `JSON.SET k $.b~1c 2`
   creates a key named `b/c`.
4. **A write that fails changes nothing.** Today a multi-match write that fails part-way leaves
   the earlier matches changed.
5. **A configuration that still loads the external module needs one edit.** With both present
   the server does not start. The operator removes the `loadmodule` line or sets
   `json-enabled no`.

One listed change was narrowed after review. A stored document nested deeper than the nesting
limit loads only up to 1,024 levels or the limit, whichever is larger. A payload also arrives
through `RESTORE`, and a replica must never hold what it is unable to load again.

Testing and review found sixteen further differences, each a case where the module's
behaviour is an accident. Six inputs that crash, hang or desynchronise the module are handled.
Appendix A section 2.5 lists all sixteen.

Everything else an operator relies on is unchanged, and Appendix A section 2.4 lists each item
with the reason.

Compatibility is a target enforced by tests, not a proof. The module's own integration suite
of 127 cases runs against the built-in: 118 pass, eight fail because of a listed change, and
one fails on the background free. A randomised comparison of 600,000 commands against the real
module found no crash and no difference outside the listed classes. Replication between the
two implementations, in both directions over 690 writes, diverged on 25 and 26 keys, every one
a listed change or a module defect.

## 6. What we do not know

**x86-64 ran on shared virtual machines.** Two Graviton3 hosts agree: on the idle one the byte
counts are identical and the median is 2.64 times. Our own x86 hosts were unreachable, so x86-64
ran on GitHub-hosted runners with 4 vCPUs. The smallest gain there, 1.03 times on reading the 100,000-record array,
is within a shared machine's noise. It was 0.97 times until a serialiser change made
serialising 10% to 25% faster.

**Eight real files are not production data.** Eight JSON files from the external module's test
data take 6% to 67% of the module's memory, with `JSON.SET` 2.1 to 2.7 times as fast. The two
at 60% and 67% are dominated by long strings, which both implementations store as they are.
Every other document is synthetic.

**The load was one server on two cores.** With 50 connections and pipelining, the built-in
serves 1.45 to 2.14 times the module's requests per second. No run used I/O
threads or cluster mode. Background save of JSON data without a fork ran only in the test
suites.

**No person has reviewed the code.** AI agents wrote the code and the tests to written
interfaces. Two AI reviewers that had not seen the code then attacked it. The engine and
number review confirmed no defect, with fuzzing under sanitizers and a thread-sanitizer run of
the key table. The command and path review found no memory-safety fault in 6 million stress
commands, and five defects, all fixed, each with a test. Neither
review replaces a person.

**valkey-search itself was not run.** A test module that uses the same interfaces the same
way gave 0 differences over 157 commands and 58 keyspace events on the two implementations. A
test module is not valkey-search.

**Fuzzing was short.** The fuzzers ran for minutes, not hours. No sanitizer build of the whole
server ran the full test suite.

**Big-endian ran under emulation only.** The JSON tests pass on 32-bit x86 on real hardware
and on s390x under qemu. Porting found two bugs, both fixed. Speed on these targets is not
measured.

## 7. What is done, and what is next

Done, on 2026-11-14 and 2026-11-15:

| Item | Evidence |
|---|---|
| Engine, path evaluator, number code, 24 commands, packaging | Zero compiler warnings with gcc and clang. clang-format 18.1.8, the version CI pins, reports no change |
| 32-bit and big-endian targets | Ported. The JSON tests pass on 32-bit x86 (1,301 tests) and on s390x under qemu |
| Compatibility with the external module | Data files and `DUMP` payloads load both ways (21 checks). 3,354 directed command steps match. Mixed-version replication and AOF run both ways. The plain C API matches in 67 calls |
| Tests in the tree | 14 files, 1,113 executions, all pass on macOS. On Linux 1,107 pass and none fail |
| Valkey's full suite with JSON built in | macOS, before the review fixes: 7,332 of 7,335, and no failure involves JSON. Linux, final code: all 7,445 tests pass, on arm64 and on an x86-64 runner |
| In-server benchmark on two Graviton hosts and x86-64 runners | Every memory target met and every operation faster on both architectures. Eight real files and a 50-connection run also measured (Appendix A, sections 4 and 15) |
| Two independent AI reviews | Five defects found and fixed (Appendix A, section 11.1) |
| Commits and documentation | Code: `5a49c3386`, `1ae595934` and `2c17a3a01`, local and on the fork's CI branch `json-builtin-ci`. `valkey-doc`: local commit `a7a40a1`. Pull-request text: `json-m0/docs/UPSTREAM_PR.md`. Nothing is pushed to valkey-io |

Next:

| Step | Exit criterion | Owner | Date |
|---|---|---|---|
| Review by people | Engine, commands and core changes each read by a maintainer | [Input needed] | [Input needed] |
| Run valkey-search itself against the built-in | Its tests pass unpatched | [Input needed] | [Input needed] |
| Longer fuzzing and a sanitizer run of the full suite | No finding open | [Input needed] | [Input needed] |
| Open the upstream proposals in valkey-io | Code and documentation pull requests opened by the author | [Input needed] | [Input needed] |

**Cost.** This document gives no effort estimate for the next steps. [Input needed:
engineer-weeks available.]

**Dependency outside this team.** One. Building the module into the server needs approval from
the Valkey Technical Steering Committee.

## 8. Risks

**The results do not hold on production data.** Eight real files point the same way as the
synthetic corpus, and that does not settle it. If a memory target is
missed, one improvement
measured in the prototype is held in reserve: sharing whole record layouts between documents,
worth a further 6% to 8%.

**Upstream declines to build JSON into the server.** The engine files include no server
header, so the fallback is an external module built from them.

**An unlisted behaviour difference breaks a client.** Sixteen were found by testing and review
and are listed. Real client traffic has not been replayed against the built-in.

**The code has defects that tests and AI review did not find.** The module is loaded in every
server, including those that store no JSON. `json-enabled no` removes it at startup. The
response is review by people, long fuzzing and a sanitizer run, all listed in section 7.

## 9. Questions and answers

**Why not a native data type, as first planned?** It breaks six things customers and tools
depend on. In a profile of `JSON.TYPE` (0.45 µs per call), the JSON code is 17% of the time.

**How much confidence do the figures deserve?** Both sides ran in the same server build, under
the same allocator, pinned to the same cores, in three interleaved rounds. The median spread
between rounds is 3.2% and the worst is 12%. A second host reproduced them, and x86-64 runners agree. The corpus is synthetic plus eight real files.

## 10. Next steps

1. Confirm the number-text decision in section 1. Owner: [Input needed]. Date: [Input needed].
2. Decide items 1 to 3 in section 1. Owner: [Input needed]. Date: [Input needed].
3. Supply the customer evidence missing from section 2, and production documents to measure.
   Owner: [Input needed]. Date: [Input needed].
4. Restore access to the x86 hosts for runs on dedicated hardware. Owner: [Input needed].
5. Name reviewers for the engine, the commands and the core changes. Owner: [Input needed].
