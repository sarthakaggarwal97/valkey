# Appendix A. JSON for Valkey, rewritten in C: technical design

Document type: technical design, appendix to the narrative in `VALKEY_JSON_NARRATIVE.md`, and
the draft of the upstream RFC. Date: 2026-11-15. Status: implemented as a built-in module,
measured inside the server on two Graviton hosts and on x86-64 hardware (GitHub-hosted runners),
tested on x86-64, 32-bit x86 and s390x, and reviewed by two independent AI reviewers. The code
is three local commits, `5a49c3386`, `1ae595934` and `2c17a3a01`, on branch
`json-builtin-module`. The branch `json-builtin-ci` of the author's fork carries the same three
commits plus one commit with the CI workflow `.github/workflows/json-builtin.yml`. Nothing is
pushed to valkey-io and no person has reviewed the code. Target release: Valkey 10. Baselines: `valkey-json` @ `60cfc5c` (the
external module, which is the specification) and Valkey `unstable` @ `9e58c3bea` (the base of
branch `json-builtin-module`). File and line references into Valkey source were taken at
`c77a4e8da`, the commit read before implementation.

This version describes the system as built. Earlier versions were written before
implementation, from a standalone prototype measured on a Mac. Those measurements stay only
where they explain a decision, and each one is labelled *prototype, superseded*. A figure
marked *measured* comes from the in-server benchmark described in §15. A figure marked
*derived* is arithmetic on measured figures or on layouts. A figure marked *published* comes
from a cited source and has not been reproduced here.

## 1. Summary

The storage engine of the valkey-json module is rewritten in C and ships inside the server
binary as a built-in module. Commands, replies, stored data, the key type and the module name
stay as they are, except for ten listed changes (B1 to B10, §2.3) and sixteen further
differences found by testing and review (C1 to C16, §2.5).

Method, in one sentence: the same Valkey source was built twice on one c7g.16xlarge host
(Graviton3, Amazon Linux 2023, gcc 11.5, jemalloc 5.3), once with JSON built in and once with
`BUILD_JSON=no` loading the external module, each server pinned to two cores, with server-side
time per call read from `INFO commandstats` over three interleaved rounds per side and the
median reported. The figures below are from the first host, which is shared. A replicate on a
second, idle host of the same type, a run of the final code on Graviton, and the same benchmark
on x86-64 GitHub-hosted runners are in §15.

Measured against the external module:

- **Memory.** 11% to 49% of the module's bytes per key on eight of nine document shapes. The
  ninth, an array of 10,000 strings of 8 to 13 bytes, is at parity (100%).
- **Time per command.** Lower on all 69 JSON operations measured. The median speed-up is 2.49
  times and the minimum is 1.09 times.
- **Persistence.** For 500,010 keys, `SAVE` takes 1.973 s against 2.469 s, and loading takes
  2.020 s against 3.097 s.
- **Replicate.** On the idle host the memory byte counts are identical and all 69 operations
  are faster, with a median of 2.64 times and a minimum of 1.14 times. The code after the
  review fixes measures 2.60 times and 1.13 times there.
- **Final code.** After the port and the serialiser change of `2c17a3a01`, all 69 operations
  are faster on both architectures. On Graviton the median is 2.69 times and the minimum 1.22
  times. On x86-64 hardware (GitHub-hosted runner, Intel Xeon 6975P-C) the median is 2.30
  times and the minimum 1.03 times, on a whole-document read of the 100,000-record array.
  Memory per shape is the same on both architectures. `SAVE` of 500,010 keys takes 1.709 s
  against 2.222 s on x86-64.
- **Platforms.** The module runs on 64-bit and 32-bit targets of either byte order. Valkey's
  full test suite passes with the module built in on x86-64. The JSON tests pass on 32-bit x86
  and on s390x (big-endian), which upstream CI builds (§10).
- **Real documents.** On eight real-world JSON files the built-in uses 6% to 67% of the
  module's memory, with `JSON.SET` 2.1 to 2.7 times and `JSON.GET` 1.4 to 4.7 times as fast.
- **Many clients.** With 50 connections, throughput is 1.45 to 2.14 times the module's.
- **Crashes and hangs.** Six inputs that crash, hang or desynchronise the external module are
  handled (§2.5). Two independent reviews led to five further fixes (§11.1).

Three things limit how far these figures generalise. The x86-64 figures come from shared
GitHub-hosted virtual machines with 4 vCPUs, whose processor changes between runs, because the
author's own x86 hosts were unreachable (§15). s390x ran under qemu emulation, so big-endian
code is tested for correctness and not measured. The documents are synthetic except for eight
real-world files from the external module's test data, which are not production data.

The engine has three parts:

1. **A packed binary encoding for small documents and small subtrees.** One allocation, a tag
   byte per scalar, a 3 or 5-byte header per container, and object keys stored as small integer
   IDs from a table shared by all documents.
2. **A pointer tree only where a container is large**, made of 8-byte tagged words, with packed
   leaves and typed arrays of integers, decimals, doubles, short strings and literals.
3. **No per-value allocation on parse or read.** Parsing goes through a reusable scratch tape
   and nothing recurses on document depth. A write to a packed node moves bytes inside the
   node, and rebuilds the node only when a size field has to widen.

What is settled and what is not:

- The representation, the key table, the number rule, the path evaluator, the 24 commands and
  the packaging are implemented and tested (§11).
- Path semantics are summarised here, not specified. The external module's selector is the
  specification. The built-in evaluator matched it on 36,205 document and path pairs (§11).
- D1 (built-in module) was directed by the owner on 2026-11-14. D2 (exact number text) was
  implemented as recommended and was not separately confirmed (§2.2).
- Done since the first implementation: x86-64 hardware measurements, the port to 32-bit and
  big-endian targets, and formatting with clang-format 18.1.8, which CI pins. Every touched
  file is clean under it.
- Not done: review by people, a run of valkey-search itself, longer fuzzing, a run on
  big-endian hardware, and opening the upstream proposals (§12).

Terms used throughout:

| Term | Meaning |
|---|---|
| External module | valkey-json at `60cfc5c`, C++ on RapidJSON, loaded with `loadmodule` |
| Built-in | The C module in `src/modules/json/`, compiled into `valkey-server` |
| Packed node | A subtree stored as one contiguous blob (§5.2) |
| Spine | The pointer tree used for containers too large to pack (§5.3) |
| Caps | The two limits a subtree must fit to be packed: bytes, and entries per container |
| Tape | A scratch array the parser fills before anything is allocated (§6) |
| Emit | Turn a finished tape into nodes. The emitter decides packed or spine |
| Splice | Edit a packed node by moving bytes inside it and patching size fields (§8) |
| Re-emit | Edit a packed node by walking it onto a tape with the change, then emitting |
| Key table | The global table that maps object keys to integer IDs (§5.5) |
| Decimal | A number in plain notation held as a scaled integer (§5.4) |
| Trusted path | Input from a source that already accepted the data: a command from a primary (`VALKEYMODULE_CTX_FLAGS_REPLICATED`), a command from an AOF file (`VALKEYMODULE_CTX_FLAGS_LOADING`), and RDB or `DUMP` payloads. The client-side checks are skipped on it and one bound applies, the load depth bound (§9) |
| Load depth bound | 1,024 levels or `json.max-path-limit`, whichever is larger. The deepest document a server accepts from any source (§9) |
| Forkless save | Core's RDB save that serialises keys on a background thread while the main thread keeps serving (`src/bgiteration.h`) |
| Gate corpus | The document shapes the acceptance gates in §4 are measured on (§15) |

## 2. Compatibility and decisions

### 2.1 Policy

Break as little as possible. A break is acceptable only when it is good for the long term, and
every break is listed so that it reaches the release notes.

Compatibility is a target held by tests, not a proof. Reading the external module turned up
behaviour that its documentation does not list. Testing against it turned up eleven more
differences, and two independent reviews turned up five (§2.5). More are expected when real
workloads run.

### 2.2 Decisions

| # | Question | Decision | Status |
|---|---|---|---|
| D1 | How the C engine ships | As a built-in module named `json`, compiled into the server and loaded at startup, using only the public module API (§10) | Decided. Directed by the owner on 2026-11-14. It reverses the 2026-11-10 choice of a native core type. Implemented |
| D2 | Number text | Exact. A number is held in binary only when its text is reproduced digit for digit (§5.4) | Implemented as recommended. It reverses the 2026-11-10 choice to normalise. The owner did not separately confirm it |
| D3 | A configuration that still loads the external JSON module | The startup option `json-enabled`, a boolean that defaults to `yes` and is immutable at run time. With `no` the server starts without the built-in, and `loadmodule` loads the external module | Implemented |
| D4 | Invalid UTF-8 in JSON text | Rejected from ordinary clients | Implemented (B1). Acceptance of the break is open |
| D5 | Results of `NUMINCRBY` and `NUMMULTBY` | Shortest form, stable type, exact integers | Implemented (B3). Acceptance of the break is open |
| D6 | A write whose matches nest inside each other | Every match is resolved against the document as it was before the command | Implemented (B4). Acceptance of the break is open |
| D7 | A new member name in a path | Literal | Implemented (B5). Acceptance of the break is open |
| D8 | A multi-match write that fails part-way | It changes nothing | Implemented (B6). Acceptance of the break is open |

B2, B7, B8, B9 and B10 in §2.3 have no decision row. B2 closes a crash. B7, B8 and B9 follow
from the new representation, and B10 from D1 and D3.

**Why D1.** Valkey `unstable` already ships one module this way. Lua is compiled in and loaded
by `moduleLoadStatic` at startup. Packaging this way keeps six things identical that a native
type changes (§2.4). The price is the module interface on every command. Before implementation
it was measured with a three-command test module on a Mac at 0.06 to 0.07 µs per command
(*prototype, superseded*). Inside the server on Graviton, a profile of `JSON.TYPE` on a random
key (0.45 µs per call) shows the JSON functions at 17% of the samples. The remaining 83% is the
server's command handling and two cache misses: the document's first bytes, and the 16-byte
wrapper the module API allocates for every module value. A single-path `JSON.GET` takes
0.52 µs where `HGET` on the same data takes 0.37 to 0.46 µs, and `JSON.NUMINCRBY` takes 0.64 µs
where `HINCRBY` takes 0.65 to 0.76 µs (measured). The engine files are the same either way, so
a later move to a native type replaces one glue file (§14).

**Why D2.** The external module keeps every non-integer number as its source text, so `19.90`
comes back as `19.90`. Normalising changes that for every client that stores money or
fixed-precision values. The rule in §5.4 keeps the text and still stores the numbers that
matter for memory in binary: a plain-notation number of up to 18 significant digits is held as
a scaled integer, so `19.90` is the pair (1990, 2) and prints back without a conversion to a
double. The print-and-compare step that made D2 expensive in the prototype now runs only for
numbers written with an exponent and for the plain-notation texts that are not decimals.

**What Q5 turned out to be.** The open question was what a write replies when a forkless save
holds its key. Every JSON command declares its keys through key specifications, so the server
blocks a write to a key in use before the command runs, and the command does not see that
case. The commands keep one guard: if opening a key for write returns NULL, the reply is
`ERROR Unable to open valkeymodule key`. The module sets
`VALKEYMODULE_OPTIONS_HANDLE_FORKLESS` and
`VALKEYMODULE_OPTIONS_HANDLE_ATOMIC_SLOT_MIGRATION`, which the external module does not set.
Core uses forkless save, and atomic slot migration, only when every loaded module sets the
matching option. The built-in module is loaded in every server, so leaving either option unset
turns that feature off for every user, including users who store no JSON.

### 2.3 Behaviour changes

Each change is described once, here. Other sections refer to it by number. All ten are
implemented as designed.

| # | Change | External module behaviour and evidence | Who notices |
|---|---|---|---|
| B1 | Invalid UTF-8 in JSON text from an ordinary client is rejected (D4) with the module's parse error | Not validated: default RapidJSON parse flags (`src/rapidjson/reader.h:141,1016`). The module then emits invalid JSON | Clients sending invalid JSON |
| B2 | A number too large for a double is rejected from an ordinary client | The reader's bound lets text such as `1.8e308` through (`reader.h:1711-1717`). A value like that once crashed servers when it was later converted (valkey-json issue 105, published) | Clients sending such numbers |
| B3 | Computed numbers (D5). A double result prints in shortest form (`2.1`, not `2.1000000000000001`). A whole double result keeps its fraction and its type (`1.5 + 1.5` replies `3.0`). An integer result is exact in value and reply | Results go through a double and `%.17g` (`src/json/util.cc:72-77`). `NUMMULTBY` turns any whole result into an integer and rounds integer products past 2⁵³ (`src/json/dom.cc:567-585,616-620`, `json.cc:1038-1062`) | Clients parsing or comparing `NUMINCRBY` / `NUMMULTBY` replies |
| B4 | Nested matches in one write are resolved against the pre-command document (D6) | `JSON.SET` applies ancestors first and re-resolves each path (`src/json/selector.cc:790-807`). On `{"a":{"a":1}}`, `JSON.SET k $..a '{"a":2}'` gives `{"a":{"a":{"a":2}}}` there and `{"a":{"a":2}}` here | Writes through `$..x` on self-nested data |
| B5 | A new member name is literal (D7): `$.b~1c` creates `b~1c` | The name passes through a JSON Pointer. `$.b~1c` creates `b/c` (pinned by `tst/integration/test_json_basic.py:4696-4699`), a name containing `/` creates nested objects, and an existing key containing `~` is impossible to set or delete | Clients writing keys containing `~` or `/` |
| B6 | A multi-match write that fails on one match changes nothing (D8) | Stops mid-loop, leaving earlier matches changed (`dom.cc:565-585`) | Clients relying on a partial update after an error |
| B7 | `json.max-document-size` counts bytes of the new representation, so one setting admits documents 2.2 to 2.6 times as large (measured: `JSON.DEBUG MEMORY` of the small records is 39% to 46% of the module's). It is checked on every write that grows a document | Checks current size plus input size, on some commands only (`dom.cc:16`) | Operators using the limit as a size bound |
| B8 | `JSON.DEBUG KEYTABLE-CHECK`, `KEYTABLE-CORRUPT`, `KEYTABLE-DISTRIBUTION`, `TEST-SHARED-API` and the memory traps are gone. A removed name replies `ERR unknown subcommand`. `MEMORY`, `FIELDS`, `DEPTH`, `MAX-DEPTH-KEY`, `MAX-SIZE-KEY` and `HELP` stay | `json.cc:2164-2334` | Module developers |
| B9 | A document nested deeper than `json.max-path-limit` still loads from RDB and `RESTORE`, up to the load depth bound: 1,024 levels or the limit, whichever is larger. The same bound applies to commands from a primary or an AOF file. A payload beyond it is refused (`ERR Bad data format` from `RESTORE`) | Rejected on load above the limit (`dom.cc:1682`), so lowering the limit makes stored data unloadable | Nobody adversely, unless stored documents are deeper than 1,024 levels and the limit is lower |
| B10 | The server has JSON without a `loadmodule` line. A configuration that also loads the external module does not start: the log shows `module name is busy` and the server aborts. The operator removes the line or sets `json-enabled no` (D3) | | Operators upgrading without editing their configuration |

**Why B9 has a bound.** The first implementation loaded any depth. The review showed two
problems with that. A payload does not only come from a file the server wrote: a client is
able to send a crafted one through `RESTORE`, and a payload of 20,000 levels made one query
use 1.6 GB. And a replica must never hold what it is unable to load again, so commands from a
primary or an AOF file are held to the same bound as a load. The bound of 1,024 keeps every
document written under the default limit of 128, or under any limit up to 1,024, loadable
after the limit is lowered.

Outputs that describe the implementation differ by nature: `JSON.DEBUG MEMORY`, `MEMORY USAGE`
and `json_total_memory_bytes` report the new, smaller sizes.

**Effect on the external module's own tests.** Its pytest integration suite has 127 cases. 118
pass against the built-in. Eight fail because of the listed changes: four on computed number
formatting (B3), one on nested matches (B4), one on member names containing `~` or `/` (B5),
and two on removed `JSON.DEBUG` subcommands (B8). The ninth, `test_big_dup`, fails because a
large document is freed on the background thread and the memory statistic settles a few
milliseconds later (C11).

### 2.4 What stays identical, and what makes it so

| Item | Stays | Because |
|---|---|---|
| `TYPE` replies `ReJSON-RL` | Yes | Same module data type name (D1) |
| `MODULE LIST`, `INFO modules`, `HELLO` show `json` | Yes | It is a module (D1) |
| Other modules see `VALKEYMODULE_KEYTYPE_MODULE` | Yes | It is a module data type (D1). valkey-search finds a module named `json` through `MODULE LIST`, takes `JSON_GetValue` from the shared API, falls back to `JSON.GET`, treats a key of module type as JSON, and subscribes to keyspace events. It compares no version number. That surface was read from its source and exercised by a test module that does the same things the same way: loaded into both implementations it gave 0 differences over 157 commands and 58 keyspace events. A test module is not valkey-search, and valkey-search itself was not run, because it is not built on this machine |
| The plain C functions that `libjson.so` exports (`is_json_key`, `is_json_key2`, `get_json_value_type`, `get_json_value`, `get_json_values_and_types`) | Yes | The server exports them with the same signatures and ownership rules (`src/modules/json/json_api.h`). A test module calling them gives identical results on both implementations in 67 cases |
| The JSON data inside RDB files and `DUMP` payloads, in both directions with the external module | Yes | Same data type name and encoding version 3 (§9). Checked both ways with the real module: 21 persistence checks pass, and a `DUMP` payload of one document is the same bytes in both implementations. Whether a Valkey 9 server reads a Valkey 10 file at all is decided by the core RDB version, as for every type |
| Statistics names such as `cmdstat_JSON.SET` | Yes | Module commands keep the case they are registered with |
| Config names `json.max-document-size`, `json.max-path-limit` | Yes | Module configs (D1) |
| Number text: `19.90`, `1e5`, 25-digit values, integers above `INT64_MAX` | Yes | D2 |
| What a write that matches nothing does. With a `$` path it still replicates and fires its event. With a legacy path it returns an error or 0 and does neither. Either way the key is signalled as modified | Yes | The built-in makes the same calls in the same cases as the module (`src/json/json.cc:1013-1033`). The signal comes from the module API when a key opened for write is closed |
| Command names, flags, key specifications, arity, events (`NOTIFY_GENERIC`, the module's names), ACL category `json`, INFO section `json_core_metrics` with `json_total_memory_bytes` and `json_num_documents` | Yes | Registered through the same module API calls with the module's values. The directed command suite compares replies with wire types, keyspace events and the replication stream against the real module: 3,354 steps, 0 failures |
| TTL: a root `JSON.SET` discards it, a sub-path write keeps it | Yes | Same API call as the module |
| Reply order and de-duplication of matches, both depth limits on commands, RESP2 and RESP3 reply shapes, error strings | Yes | Reproduced from the selector (§7) |

### 2.5 Module defects that are not reproduced, and further differences found by testing

**Defects found by reading the module.** A client is not able to rely on these today. The
test generators avoid them (§11).

- Nested matches in the array commands, `CLEAR` and `MERGE` use pointers taken before the first
  mutation (`dom.cc:902,952,1035`) and read freed memory.
- `JSON.MSET` naming one key twice, where the first change invalidates the second, hits an
  assert (`json.cc:832,847`).
- `JSON.SET` with exactly one update and at least one insert moves the new value into the
  update and then inserts what is left (`selector.cc:787`, `:845`).
- `STRAPPEND` cuts both operands at an embedded NUL (`dom.cc:742-746`).

**Further differences found by testing and review (C1 to C18).** C1 to C11 were found by
running the built-in against the real module, C12 to C16 by the independent review
(§11.1), and C17 and C18 by a three-hour differential run that first found them as a bug in the
built-in (§11). In each, the module's behaviour is an accident, not a contract. The
random differential test reports 99 to 154 differences per seed of 50,000 commands outside B1
to B10, and every one falls in a class below.

| # | Case | External module | Built-in |
|---|---|---|---|
| C1 | JSON text with an embedded or trailing NUL byte | Stores the text before the NUL | Error for an ordinary client. On a trusted path the text ends at its first NUL, as the module reads it (§9) |
| C2 | A member name containing NUL | The member is impossible to delete | Works |
| C3 | `JSON.RESP` of a number that is not an int64 integer | Prints the double with 17 digits: `16.424925` becomes `16.424925000000002` | The shortest digits of the value: `16.424925`. As in the module, this is the value and not the stored text: a stored `19.90` replies `19.9` |
| C4 | `-5` multiplied by `0` | `-0` | `0` |
| C5 | Slice step 2147483648 or 4294967296 | Never returns | First element |
| C6 | Slice step with bit 31 set, or an index at or above 2⁶⁴ | Wraps | First element for the step. Out of range for the index |
| C7 | `JSON.RESP` with a legacy path matching several values | One reply per match, which breaks the protocol | First match |
| C8 | `JSON.SET k $ 1.8e308`, then `JSON.NUMINCRBY` | Server aborts | Rejected at `JSON.SET` (B2) |
| C9 | `[5e-324]`, then `JSON.RESP` | Server stops serving. The results write-up records an abort and the test comment a hang | Works |
| C10 | `JSON.DEL` of several values in a document with a `~` in a name | Server crashes | Works |
| C11 | `json_total_memory_bytes` right after deleting a large document | Exact at once | Settles within milliseconds, because the free runs on the background thread (§10) |
| C12 | `JSON.DEBUG MAX-SIZE-KEY` and `MAX-DEPTH-KEY` | Reply the key name as a simple string. A key name containing CR LF injects extra replies | Reply it as a bulk string |
| C13 | A read through a path that chains a repeated union member, such as `['a','a']` 22 times | Work doubles with every step: 1.07 s and a 209 MB reply at 22 steps | A few repeats reply as before. Beyond a work bound the command fails in 1.6 ms with `LIMIT Total number of repeated matches in the query result exceeds the limit` (§7) |
| C14 | `JSON.GET` with `INDENT`, `NEWLINE` or `SPACE` whose reply exceeds 512 MB | Builds it | `LIMIT Formatted JSON reply size limit is exceeded`, before the reply is built (§6.4) |
| C15 | A new member name that is not valid UTF-8, given through a path | Accepted | Rejected for an ordinary client with the parse error, as B1 rejects it in JSON text. A read or a delete through such a path matches nothing |
| C16 | Recursive descent (`..`) on a document more than 4,096 levels deep | No limit. The process dies at 20,000 levels | `LIMIT Parser recursion depth is exceeded` once the descent is 4,096 levels below its start (§7) |
| C17 | A quoted name after a comma in a union, or a quoted value on the left of a filter comparison, that starts with spaces: `$['x',' a']`, `$[?(' a'==@.n)]` | Drops the leading spaces: looks up `a`, compares with `"a"` | Keeps them |
| C18 | A number on the left of a filter comparison with a space inside: `$[?(5 0!=@.n)]` | Reads it as 5 and ignores the rest | `SYNTAXERR Invalid JSON path` in every mode |

Six of these are inputs that crash, hang or desynchronise the external module. The write-up
gives the count and not the list. By the descriptions above they are C5, C7, C8, C9, C10 and
C12. The module also has the defects behind C13 and C16.

**The plain C symbols.** `libjson.so` exports five C functions declared in its `json_api.h`.
The built-in implements all five in `json_cmd.c` with default visibility, declared in
`src/modules/json/json_api.h`, so another module resolves them from the server binary as it
resolved them from the module's shared library. They are tested (§2.4, §11).

### 2.6 Open questions

| # | Question | Outcome |
|---|---|---|
| Q1 | The reply for a match whose effect is dropped under D6 | Computed from the pre-command document, as if that match were applied alone. Implemented |
| Q2 | `MERGE` and `CLEAR` with nested matches | Treated as replacing writes: the outermost match wins, and every match is counted in the reply. Implemented. The module has no defined behaviour here |
| Q3 | The parser: hand-written, or yyjson's reader in front of the emitter | Hand-written. yyjson was not evaluated. With the hand-written parser, whole-document `JSON.SET` is 2.30 to 2.49 times the module's speed on the small records (measured), so the question is closed for this version |
| Q4 | The source of the number printer (§5.4) | Answered. A Schubfach printer and an Eisel-Lemire parser with an exact fallback, written for this module in `json_num.c` (§6.5) |
| Q5 | What a write replies when forkless save holds its key | Answered in §2.2. The server blocks the write before the command runs |
| Q6 | Whether to become a native type later | Not now. §14 lists what it changes |

## 3. Baseline: what valkey-json does today

Paths are relative to the valkey-json checkout.

| Area | Behaviour | Reference |
|---|---|---|
| Value node | RapidJSON `GenericValue`, 16 bytes | `src/rapidjson/document.h:2232-2240` |
| Strings | Up to 13 bytes inline in the node. Longer ones a heap allocation of length + 1 | `document.h:2177-2185,2968` |
| Numbers | Integer syntax within int64 in binary. Every other number as its source text, with the string rule above. Integers above `INT64_MAX` are text too | `document.h:2133-2136`, `reader.h:1604-1616` |
| Arrays | Heap vector of 16-byte nodes, 1.5× growth | `document.h:1810` |
| Objects | Vector of 24-byte members. Above 64 members, a hash table with 32-byte members | `document.h:174-198`, `src/json/json.cc:78` |
| Key interning | Global `KeyTable`, 32,768 shards, one mutex per shard taken on every insert and release | `src/json/keytable.h:1-60,167-171` |
| Path evaluation | Recursive, guarded by a fixed counter of 200 | `src/json/selector.cc:40-55` |
| Document nesting | Limited by `json.max-path-limit` (128) on parse, on RDB load and on sub-path writes | `document.h:3410-3414`, `src/json/dom.cc:27-38,1682` |
| Read path | Serialises into a buffer, then copies it into the reply | `src/json/dom.h:39-49` |
| RDB | Encoding version 3: the document as one JSON text string. Version 0: node by node. No aux data | `dom.cc:1547-1553,1671-1686` |
| AOF rewrite | `JSON.SET key . <text>` | `json.cc:2450-2455` |
| Defrag | Copies the whole document in one call if it is under 64 MB. Its maintainers report that the copy receives the memory it just freed (valkey-json issue 26, open since 2024-12-11) | `json.cc:61,2426-2445` |
| Forkless save | Not supported: the module does not set the forkless option, so the server falls back to fork | `json.cc:2744` |
| Free | On the main thread. Deleting a document of 100,000 records blocks every client for 28 ms (measured, §4) | |
| Shared API | `JSON_GetValue(key, path, &string)`. `libjson.so` also exports plain C symbols | `src/json/shared_api.cc:14-33`, `src/json/json_api.h:15-64` |
| Configs | `json.max-document-size` (0 = unlimited, skipped for replicated commands), `json.max-path-limit` | `json.cc:60-78`, `dom.cc:18` |
| Commands | 24: SET GET MGET MSET MERGE DEL FORGET TYPE RESP CLEAR TOGGLE NUMINCRBY NUMMULTBY STRLEN STRAPPEND OBJLEN OBJKEYS ARRLEN ARRAPPEND ARRINSERT ARRINDEX ARRPOP ARRTRIM DEBUG | `src/commands/` |
| Size | 8,000 lines of C++ (rounded) in `selector.cc`, `dom.cc`, `json.cc` and `keytable.cc`, on top of RapidJSON | |
| Tests | gtest unit (dom 127, selector 31), pytest integration (127 cases with the RDB test) | `tst/` |

The external module measured on the Graviton host (§15). Memory is `JSON.DEBUG MEMORY` in
bytes. Times are server-side microseconds per call.

| Shape | Text, bytes | Memory, bytes | `JSON.SET`, µs | `JSON.GET`, µs |
|---|---:|---:|---:|---:|
| Session record | 307 | 519 | 4.05 | 1.90 |
| Profile record | 596 | 972 | 6.27 | 2.52 |
| Order record | 747 | 1,245 | 8.96 | 3.23 |
| Array of 100,000 records | 8,107,476 | 17,835,032 | 90,780 | 14,428 |
| 1,536 doubles at full precision | 32,142 | 61,464 | 284.32 | 67.48 |
| 1,536 values with six decimals | 14,593 | 24,600 | 90.47 | 32.00 |
| 100,000 integers | 1,048,299 | 1,835,032 | 4,029 | 2,792 |
| 10,000 strings of 8 to 13 bytes | 134,802 | 163,864 | 311.45 | 83.96 |
| Object with 100 integer members | 1,584 | 4,120 | not measured | not measured |

Single-path operations on the profile record take 1.42 to 2.25 µs each in the module:
`JSON.GET` of one field 1.45 to 2.25, `NUMINCRBY` 1.69, `JSON.SET` of a longer string 1.55,
`STRAPPEND` 1.42, `ARRAPPEND` 1.73. `JSON.GET` of one member of the 100-member object takes
0.99 to 1.03 µs. `JSON.TYPE` of one field takes 1.15 µs. Each key also costs bytes outside the
document: the growth of `used_memory` per key exceeds `JSON.DEBUG MEMORY` by 61 to 65 bytes
with the module and by 53 to 57 bytes with the built-in (derived from the table in §5.2).

## 4. Goals, non-goals and acceptance gates

Goals

- Break as little as possible (§2). Run the external module's integration tests with changes
  only where §2.3 lists one.
- Lower memory on every gated shape.
- Faster parse, whole-document read, and path read and write.
- No stack recursion on user-controlled depth.
- No unbounded main-thread work that the module does not already have. The one exception is
  stated where it occurs (§8.4).
- Simple means one mechanism per job, not fewer wins. An optimisation that serves a gate stays.
  Machinery that serves no gate stays out.
- Run on every target upstream CI builds: 64-bit and 32-bit, little-endian and big-endian
  (§10).

Non-goals for this version

- New commands or new path syntax.
- The module's diagnostics (B8).
- A new RDB format. The module's text format stays (§9).
- Sharing whole object shapes across documents, float32 storage, and splitting giant arrays
  into chunks. Each was measured or researched before implementation and is recorded in §14.
- Speed on 32-bit and big-endian targets. They are tested for correctness and not measured
  (§11).

Acceptance gates. The results column is measured inside the server against the external module
(§15). Every gate is met on the shapes measured. One gate names documents up to 4 KB and was
measured on a 596-byte record.

| Shape or operation | Gate | Measured in the server | Met |
|---|---|---|---|
| Memory, small records (session, profile, order) | ≤ 50% of module | 49%, 49%, 41% of `used_memory` growth per key. 44%, 46%, 39% of `JSON.DEBUG MEMORY` | Yes |
| Memory, array of 100,000 records | ≤ 60% of module | 33% | Yes |
| Memory, 1,536 doubles written in 16 or more characters | ≤ 25% of module | 23% | Yes |
| Memory, 1,536 doubles written in 9 characters or fewer | ≤ 60% of module | 29% | Yes |
| Memory, 100,000 integers in int32 range | ≤ 30% of module | 25% | Yes |
| Whole-document parse and serialise, small records | ≥ 1.3× module | `JSON.SET` 2.30×, 2.43×, 2.49×. `JSON.GET` 3.11×, 3.00×, 3.08× | Yes |
| Single-path read and in-place number write | ≥ module | `JSON.GET` of one field 2.46× to 4.02×. `NUMINCRBY $.age` 2.64×. `JSON.SET $.age` 2.53× | Yes |
| A single-path write that changes the encoded size, on documents up to 4 KB | ≥ module | On the 596-byte profile record: `JSON.SET $.name` 1.82×, `STRAPPEND` 1.58×, `ARRAPPEND` 1.82×, `ARRINSERT` at 0 1.58×, add a member 2.45×, delete a member 2.74×. The 3.8 KB document of the prototype runs was not in the in-server benchmark | Yes, on the record measured |
| Member lookup in an object of 65 to 128 members | ≥ module | 100-member object: `JSON.GET` of member 50 2.54×, of member 99 2.02×, `NUMINCRBY` on member 99 1.85× | Yes |
| Parse and serialise, large documents | ≥ module | 100,000 records: `JSON.SET` 2.08× (43.6 ms against 90.8 ms), `JSON.GET` 1.09× (13.2 ms against 14.4 ms). Final code on x86-64 hardware: `JSON.GET` 1.03× (10.55 ms against 10.91 ms) | Yes. 1.09× is the minimum of the 69 ratios on this host. On x86-64 the gate was missed at 0.97× until the serialiser change of `2c17a3a01` (§6.4) |
| Parse, number-heavy documents | ≥ module | `JSON.SET`: full-precision doubles 3.01×, six-decimal values 1.75×, integers 1.43× | Yes |
| Serialise, number-heavy documents | ≥ module | `JSON.GET`: full-precision doubles 2.57× (26.30 µs against 67.48 µs), six-decimal values 2.48× (12.90 µs against 32.00 µs), integers 2.08× | Yes |

Seven notes on reading the table.

- **Gates that were missed in the prototype and are met now.** Serialising number-heavy
  documents was missed for doubles: 118 µs against the module's 72 µs for full precision and
  51 µs against 36 µs for six digits (*prototype, superseded*). An early in-server run still
  showed 0.51× on the six-decimal array and 0.93× on the full-precision one. Decimals (§5.4)
  and the new printer (§6.5) moved them to 2.48× and 2.57×. The six-decimal array also moved
  from 58% to 29% of the module's memory.
- **The one shape at parity.** A large array of short strings has no gate. It was 148% of the
  module's memory in the prototype and in the first in-server run. Arrays of 16-byte inline
  strings (§5.3) bring it to 100% (163,848 bytes against 163,864 by `JSON.DEBUG MEMORY`), with
  `JSON.SET` at 1.70× and `JSON.GET` at 1.23× the module's speed. It is the only shape with no
  memory gain.
- **Margins.** The session and profile records sit one point inside the 50% gate when the key
  name and the keyspace entry are counted. The `JSON.GET` ratio of 1.09 on the record array is
  inside the worst run-to-run spread measured for a single operation on this host (12%). The
  same read is 1.03× on x86-64 hardware, inside the noise of a shared virtual machine.
- **Replicate.** On the second, idle host all 69 operations are faster, the median is 2.64×
  and the minimum is 1.14×. Memory byte counts are identical on all nine shapes. With the
  review fixes the code measures 2.60× and 1.13× there (§15).
- **Final code.** On Graviton the median is 2.69× and the minimum 1.22×. On x86-64 hardware
  the median is 2.30× and the minimum 1.03×, with the same memory per shape (§15).
- **Beyond the gate corpus.** Two further measurements are in §15: eight real-world files
  (6% to 67% of the module's memory) and 50 concurrent connections (1.45 to 2.14 times the
  module's requests per second on Graviton, 1.33 to 1.93 times on x86-64).
- **Deleting the large document.** `JSON.DEL` of the 100,000-record key takes 18.65 µs against
  28,020 µs. That is a change of behaviour, not of speed: the built-in reports how much work a
  free is, so the server frees a large document on its background thread (§10).

The gates compare against the module, not against stock RapidJSON.

## 5. Data representation

The byte formats are documented at the top of `src/modules/json/json_engine_format.h`. Only
`json_engine.c` writes them. Neither encoding is ever written out, because a document is saved
as JSON text (§9). So both are in the machine's byte order: every multi-byte field, and every
integer the engine takes from string bytes, is written and read through one set of helpers
(`loadU16` to `storeU64` in `json_engine_format.h`). A spine word is 64 bits wide whatever the
width of a pointer. Two places depend on where the bytes of a word lie, and each names it: an
inline string (§5.3), and code that treats eight bytes as one integer with the first byte
lowest, to find a byte by counting bits or to hash them as SipHash defines it (`loadU64LE`).
The engine supports 64-bit and 32-bit targets of either byte order, and refuses to compile
for any other byte order.

### 5.1 One representation

A document is a tree of nodes in which any subtree that fits the caps is a single packed node.
A small document is the case where the root is one packed node. There is one code path.

- Caps: `json.max-packed-bytes` (default 4096, range 0 to 65535) and `json.max-packed-entries`
  (default 128, range 0 to 65535, per container). A change applies to nodes built afterwards.
  A cap of 0 packs nothing, not even an empty container.
- In the gate corpus no small record reaches 1 KB packed and no container has more than
  10 members, so these defaults leave every small record as one allocation: 229, 449 and 488
  bytes for the session, profile and order records (measured, `JSON.DEBUG MEMORY`). Lowering
  the bytes cap below a document's size costs memory. In the prototype the order record was
  535 bytes at a cap of 1024 or above, 598 at 512 and 772 at 256 (*prototype, superseded*).
- The emitter (§6) is the only code that decides packed or spine.
- A spine container stays a spine container when it shrinks. It gives capacity back when it
  is under a quarter full. A root `JSON.SET`, `COPY` and an RDB load emit from scratch, so the
  result is shaped by the caps in force at that moment.
- Where a pointer is 8 bytes, a key's value pointer holds the document word itself, and the
  module allocates nothing for a document beyond the document's own nodes. On a 32-bit target
  the value pointer is the address of an 8-byte heap cell that holds the word
  (`JSON_DOC_BOXED` in `json_module.h`). Building with `-DJSON_DOC_BOXED=1` forces the cell on
  a 64-bit target, which is how that form is tested there.

### 5.2 The packed format

A value is a tag byte and a payload. It holds no pointers, so a packed subtree moves or copies
as bytes.

| Value | Tag (hex) | Payload | Bytes |
|---|---|---|---|
| null, false, true | `00`, `01`, `02` | none | 1 |
| Integer 0 to 31 | `20` to `3F` | none, the value is in the tag | 1 |
| int8, int16, int32, int64 | `03` to `06` | two's complement integer, machine byte order | 2, 3, 5, 9 |
| Double | `07` | IEEE-754, machine byte order | 9 |
| Decimal with 1 to 15 fraction digits (§5.4) | `C0` to `FB`, the tag carries the scale and the width | its digits as an integer of 1, 2, 4 or 8 bytes | 2, 3, 5, 9 |
| Decimal with 16 to 31 fraction digits | `FE` | scale in 1 byte, digits in 8 bytes | 10 |
| Raw number (§5.4) | `FC`, `FD` | 1 or 2-byte length, then the source text | 2 + n, 3 + n |
| String up to 63 bytes | `40` to `7F` (escaping needed on output), `80` to `BF` (no escaping needed) | the bytes | 1 + n |
| Longer string | `08`, `09` (no escaping needed) | 16-bit length, then the bytes | 3 + n |
| Array | `0A`, `0B` | byte size and count in 1 byte each or 2 bytes each, then values | 3 + size, 5 + size |
| Object | `0C`, `0D` | byte size and count as for an array, then (key, value) pairs | 3 + size, 5 + size |
| Typed array | `0E` to `1F`, the tag carries the element kind and the count width | count in 1 or 2 bytes, then raw elements. A decimal kind has one scale byte before the elements | 2 or 3 + n × width, plus 1 for decimals |
| Key | none | varint key-table ID, 7 bits per byte, low bits first. 1 byte for IDs up to 127. ID 0 is never used, so a key never starts with `00` | 1 to 5 |

Element kinds of a packed typed array: integers of 1, 2, 4 or 8 bytes, doubles of 8 bytes, and
decimals that share one scale, as their digits in 1, 2, 4 or 8 bytes.

Properties the rest of the design relies on:

- **Skippable.** Every container carries its byte size, so stepping over a sibling is O(1).
- **Ordered.** Members and elements keep insertion order, as in the module.
- **Unescaped strings**, with a no-escape flag in the tag set at parse time when the bytes
  need no escaping, so the serialiser copies them. Bytes that are not valid UTF-8 are present
  only when they arrived on a trusted path (B1 applies to client input) and are written out
  unchanged.
- **No pointers.** A subtree is packed whole or not at all.
- A blob is at most 65,535 bytes, so size and count fields are 8 or 16 bits. Every field
  wider than a byte is in the machine's byte order.
- A field is allowed to be wider than its value needs. An integer or the digits of a decimal
  left in a wider field by an in-place write, and a size field left wide after a shrink, are
  both valid. SQLite's binary JSON uses the same rule to avoid moving bytes (published).

**Keys and values are interleaved.** A keys-first layout (all key IDs, then value offsets, then
values) was measured in the prototype and rejected. It cost 3% to 9% more memory on small
records and made a lookup 5 to 60 ns faster (*prototype, superseded*). In the server, reading
member 99 of a 100-member object takes 0.51 µs for the whole command, against 0.39 µs for
member 50 (measured).

**Measured sizes** in the server, bytes per key. `used_memory` is the growth of the server's
`used_memory` over 100,000 keys, so it includes the key name and the keyspace entry. For the
single documents it is `MEMORY USAGE`.

| Shape | Text | Module `used_memory` | Built-in `used_memory` | Module `JSON.DEBUG MEMORY` | Built-in `JSON.DEBUG MEMORY` |
|---|---:|---:|---:|---:|---:|
| Session record | 307 | 580 | 282 | 519 | 229 |
| Profile record | 596 | 1,037 | 506 | 972 | 449 |
| Order record | 747 | 1,308 | 541 | 1,245 | 488 |
| Array of 100,000 records | 8,107,476 | 17,835,056 | 5,861,024 | 17,835,032 | 5,861,008 |
| Object with 100 integer members | 1,584 | 4,182 | 450 | 4,120 | 392 |

The built-in stores less than the JSON text itself on eight of the nine benchmark shapes. In
the prototype, storing key text inline in place of key IDs put the small records at 55% to 57%
of the module (*prototype, superseded*). Key IDs are what brings them under the gate.

### 5.3 The spine

A value slot is one 64-bit word. The tag is in the low 3 bits.

| Low bits | Meaning |
|---|---|
| `000` | Pointer to a heap node, whose first byte is its kind. The allocator aligns every block to 8 bytes or more on every target, 32-bit ones included, which leaves the three bits free. Where a pointer has 32 bits it is the low half of the word |
| `001`, `101` | Inline double: the word is the IEEE bits rotated left by 3, so the tag is the sign and the top two exponent bits. Covers magnitudes in [2⁻⁵¹¹, 2) |
| `010`, `110` | Inline decimal, positive or negative. The upper 61 bits hold the magnitude of its digits times 22, plus the scale minus 1. Covers scales 1 to 22 and every digit string of 17 digits |
| `011` | Inline signed 61-bit integer |
| `100` | Inline string of up to 7 bytes. The low byte of the word holds the tag, the length and two flags (no escaping needed, raw number text). The string is in the other seven bytes, in memory order, so a reader gets a pointer into the word. One helper locates it on both byte orders (`W_STR_AT`, `wordStrBytes`): bytes 1 to 7 of the word's memory on little-endian, bytes 0 to 6 on big-endian. Such a word is compared only whole, so nothing depends on its value as an integer, which differs between byte orders |
| `111` | null, false, true, +0.0, −0.0 or, in an object entry, a key ID in the upper 32 bits |

Before decimals existed, four tags carried inline doubles and covered [2⁻⁵¹¹, 2⁵¹³). Two of
them now carry decimals. A double of magnitude 2 or more that is not a decimal is boxed.

Heap nodes. Every multi-byte field is in the machine's byte order.

| Kind | Layout | Notes |
|---|---|---|
| `STR` | 1 byte of kind and flags. Length in 1 byte up to 255 bytes and in 4 bytes above. Then the bytes | Strings over 7 bytes, and raw numbers (a flag marks them) |
| `BOXNUM` | 16 bytes: kind, a type byte (int64, double or decimal), the scale of a decimal, and the 8-byte value | Integers beyond 61 bits, doubles outside the inline range, and decimals a word does not hold: 18 digits, or a scale above 22 |
| `PACKED` | kind, 2-byte blob length, one packed value | Any subtree within the caps. The leaves of a large document |
| `ARRAY` | 32-byte header (length, capacity, node count, byte count), then one word per element | 8 bytes per element, 1.5× growth |
| `TARRAY` | 16-byte header (element kind, scale of a decimal kind, length, capacity), then raw elements | See the element kinds below |
| `OBJECT` | 32-byte header, then insertion-ordered entries of key word and value word, then an open-addressing index of entry numbers (1, 2 or 4 bytes each) | O(1) lookup. A removed member leaves an entry whose key word is 0 until the node is rebuilt. Index deletion shifts entries back, so the index has no tombstones. CPython's dictionary has this shape (published) |

The node count and byte count of an `ARRAY` or `OBJECT` are maintained only in the root node
of a document. They give `MEMORY USAGE` and the free effort in O(1).

**Typed arrays.** A `TARRAY` holds elements of one kind as a plain C array:

| Element kind | Width, bytes | In a packed node | In a spine node |
|---|---|---|---|
| Integers: i8, i16, i32, i64 | 1, 2, 4, 8 | Yes | Yes |
| Decimals of one scale, with the same four widths for their digits | 1, 2, 4, 8 | Yes | Yes |
| Doubles (f64) | 8 | Yes | Yes |
| Strings of up to 15 bytes. Byte 0 is the length and the no-escape flag, the string follows | 16 | No | Yes |
| Literals: null, false, true, each as its packed tag | 1 | No | Yes |

The emitter picks a numeric typed array when a non-empty array holds only integers, only
doubles, or only decimals that share a scale. The integer or decimal width is the narrowest
that holds every element. When an array does not fit a packed node, the emitter picks a string
array if every element is a string of up to 15 bytes and one or more of them is longer than
the 7 bytes a word holds, and a literal array if every element is null, false or true.

A typed array widens within its family when a write needs it, like `intset`. It becomes a
plain `ARRAY` when a write inserts a decimal of another scale, a number of another kind, or a
value that is not a number. No element changes type.

Measured costs in the server (`JSON.DEBUG MEMORY`):

- An array of 100,000 records is one `ARRAY` of pointers to packed records: 5,861,008 bytes
  against the module's 17,835,032 (33%).
- 100,000 integers in int32 range are one `TARRAY` of i32: 458,760 bytes, 4.6 bytes per
  element, against the module's 1,835,032 (25%).
- 1,536 values written with six decimals are one `TARRAY` of 4-byte decimals: 7,176 bytes
  against the module's 24,600 (29%). The 4-byte width is derived from the size. Before decimals
  this shape was 14,336 bytes of doubles.
- 1,536 doubles at full precision take 14,392 bytes against the module's 61,464 (23%).
- 10,000 strings of 8 to 13 bytes are one `TARRAY` of 16-byte strings: 163,848 bytes, 16.4
  bytes per element, against the module's 163,864 (100%). With one `STR` node per string the
  same array was 241,920 bytes, 148% of the module.
- An array of only `true`, `false` and `null` costs 1 byte per element as a literal array,
  against 8 bytes as words and 16 in the module (derived from the layouts).
- A 100-member object of small integers is 392 bytes packed. As a spine `OBJECT` the prototype
  measured 2,048 bytes (*prototype, superseded*). This is the size cliff at the entries cap
  (§13).

### 5.4 Numbers

A number is one of four kinds. The rule keeps every number's text exact (D2).

| Kind | When | Stored as | `JSON.TYPE` |
|---|---|---|---|
| Integer | Integer syntax (no fraction, no exponent) within int64. `-0` is integer 0, as in the module (`reader.h:1767-1778`) | Binary, narrowest width | `integer` |
| Decimal | Plain notation with a fraction: no exponent, 1 to 31 fraction digits, and no more than 18 digits counted from the first non-zero digit to the last digit. A negative zero is excluded | The integer M that the digits spell without the point, with \|M\| < 10¹⁸, and the scale k: `19.90` is (1990, 2) | `number` |
| Double | Any other number whose source text is exactly the canonical text of the double it parses to | IEEE-754 binary | `number` |
| Raw | Every other valid JSON number: `1e5`, `1.0E3`, more than 18 digits, integer syntax beyond int64, `-0.00` | The source text, verbatim | `number` |

**Decimal.** The text of a decimal is M with the point put back k digits from the right, so it
is reproduced exactly without printing or parsing a double. Trailing zeros are part of the
scale: `19.90` and `19.9` are different decimals with the same value. The value is M / 10ᵏ,
correctly rounded. It is one exact division when \|M\| ≤ 2⁵³ and k ≤ 22. Otherwise the number
library converts it (`jsonDoubleFromDecimal`, §6.5). Outside the engine a decimal is reported
as a number that has a text of its own, the same class as a raw number, so the command code
needed no change for it.

**Canonical text** of a double is built from its shortest digit string, the fewest digits that
read back as the same double:

- Zero is `0.0`, and negative zero is `-0.0`.
- A whole value below 1e21 in magnitude is its digits followed by `.0`: `100000.0`.
- Any other value from 1e-6 up to 1e21 in magnitude is plain decimal: `0.1`, `98.6`.
- Everything else is exponent form as ECMAScript writes it: no `.0`, a sign on the exponent,
  no leading zero in it. For example `1e-7` and `1.5e+21`.

Plain-notation texts of up to 18 digits are decimals before this rule is consulted. The Double
kind therefore holds what Decimal does not cover: canonical exponent forms such as `1e-7` and
`1.5e+21`, `-0.0`, and whole values with 18 to 21 integer digits written with `.0`, such as
`100000000000000000000.0`.

**Deciding at parse time.** The parser classifies a number token in this order.

1. Integer syntax within int64 is an Integer. Integer syntax beyond int64 is Raw.
2. No exponent, at most 31 fraction digits, at most 18 digits from the first non-zero one, and
   not a negative zero: Decimal. No conversion to a double and no printing happens.
3. No exponent and at most 15 significant digits: the text is canonical if it is also laid out
   canonically, meaning plain notation is right for its magnitude and the fraction has no
   trailing zero other than a lone `.0`. A decimal string of at most 15 digits is the shortest
   text of its double, so no printing is needed. Canonical means Double, otherwise Raw. After
   step 2 this step decides `-0.0` (Double), `-0.00` (Raw) and whole values with 18 to 21
   integer digits written with `.0` (Double).
4. At most 17 significant digits, with or without an exponent: the number is parsed, printed
   canonically and compared with the source. Equal means Double, otherwise Raw.
5. Anything else is Raw.

One number does not keep its text. `-0` in integer syntax is stored as integer 0 and prints as
`0`. The module does the same, so no client sees a change.

**The per-array format is gone.** Earlier versions of this design gave every array one format
byte, "canonical" or "fixed with k fraction digits", so that an embedding written with `%.6f`
stayed in a typed array of doubles. That format was never shipped. Decimals replace it: an
array of values with six fraction digits is a typed array of decimals with scale 6, at 4 bytes
per element where the fixed-k array used 8.

**Range.** A number that is not finite when converted is rejected from an ordinary client (B2).
On a trusted path its text is kept as a raw number, since nothing is rejected there.
Arithmetic on it is an error.

**Arithmetic** (B3). A decimal or raw operand is converted when used, as the module converts
its text. Integer with integer gives an exact integer if the result fits int64. Otherwise, or
if either operand is not an integer, the result is a double. A computed double is stored as
what its canonical text is read as: a decimal when that text is short plain notation (`2.1`,
`3.0`), and the double otherwise (`1.5e+21`). Either way the reply is the canonical text. A
result that is not finite is an error, as in the module. A result written into a typed array
that is unable to hold it widens the array or turns it into a plain array (§5.3).

**What the rule costs, measured in the server.** On the number-heavy shapes `JSON.SET` runs at
3.01× (full-precision doubles), 1.75× (six decimals) and 1.43× (integers) the module's speed,
and `JSON.GET` at 2.57×, 2.48× and 2.08×. The printer and parser behind the Double and Raw
kinds cost 26 to 35 ns per number (§6.5).

**History: why the printer was replaced.** Valkey's `fpconv_dtoa` is Grisu2, which round-trips
but is not always shortest. In the prototype it printed 328 of 300,000 two-to-six-digit
decimals longer than their source, for example `16.424925` as `16.424925000000002`, and
printing every 16 or 17-digit number once at parse and once at serialise added 258% to parse
time and 45% to serialise time on the full-precision embedding. Reading back an array of
doubles took 118 µs against the module's 72 µs (*prototype, superseded*). An early in-server
run showed that printing doubles was 86% of reading an array of decimals. Two changes removed
the cost: decimals, which need no printing, and the Schubfach printer of §6.5.

### 5.5 Key table

One global table maps object keys to 32-bit IDs. It is the only state shared between JSON keys,
so the threading rules live here.

*What it buys and costs.* In the prototype, storing key IDs in place of key text took the small
records from 55% to 57% of the module's memory down to 40% to 46%. It added close to 20% to
parse time, and the key cache of §6.1 brought parse time to within 5% of not interning at all
(*prototype, superseded*). In the
server the small records are at 39% to 46% of the module by `JSON.DEBUG MEMORY`, and
whole-document `JSON.SET` is 2.30 to 2.49 times the module's speed with interning included.

*Structure.*

- **Slots.** A slot holds a reference count, the key's length and no-escape flag, its hash,
  a pointer to the key bytes, and the length of the key's serialised form. Slots live in
  segments that double in size and never move:
  segment s holds 64 × 2ˢ slots, and a fixed directory of 26 segments covers every ID below
  2³² − 64. Going from an ID to a slot is a bit scan and two loads, with nothing that a reader
  sees reallocated.
- Within a segment the counts are one array and the read-only fields another. Background
  readers touch only the second, and the sweep below scans only the first.
- **The count is an atomic `size_t`.** Each reference occupies one or more bytes of some
  document or tape, so it does not overflow and there is no saturation rule. `SIZE_MAX` marks
  a free slot.
- **Key bytes** are a separate allocation, immutable while the count is above zero, and are not
  defragmented. The allocation also holds the key's serialised form: the name quoted and
  escaped as the serialiser escapes it, then a colon. A name that needs no escaping is stored
  once, inside its serialised form `"name":`, so it costs its length plus 3 bytes. A name that
  needs escaping is stored as it is, followed by its serialised form, so it costs its length
  plus its escaped length plus 3. The serialiser writes a member name as one copy of that form
  (§6.4).
- **Lookup by bytes** is an open-addressing hash index over key IDs, used only on the main
  thread. The hash is SipHash with a 16-byte key that the glue takes from the server's random
  source at load, because member names come from clients. It reads the name as SipHash defines
  it, with the first byte lowest, on both byte orders. The first version read the name and the
  seed in native order, so on big-endian names that differed only in their last bytes
  collided. The port found and fixed this.
- **The lowest free ID is handed out first**, tracked by one bit per slot. That favours
  one-byte IDs for keys interned early. It does not guarantee them: an ID is fixed for the
  life of its key.

*Who touches it.* "Main thread" includes a module thread holding the GIL.

| Thread | Does | Safe because |
|---|---|---|
| Main | Intern, increment, decrement, free, sweep | Sole writer of the hash index, the free maps and slot contents |
| BIO (lazy free, async flush) | Decrements counts by ID | Atomic, on a slot that does not move. A count reaching zero sets one atomic dirty flag |
| Forkless-save reader | Reads key bytes by ID for a key the main thread is blocked from writing | The document holds a reference, so the slot is live and its bytes immutable |
| Fork children | Read | Copy-on-write |

*Freeing.* The engine records the thread that initialised it and compares against it. A count
that reaches zero on the main thread frees the slot at once. One that reaches zero elsewhere
leaves the slot live at zero and sets the dirty flag. A slot at zero is still in the hash
index, so interning its name again revives it. `jsonKeyTableSweep`, called from the server's
cron event, frees such slots later. One call examines at most 4,096 slots and reports whether
more remain.

*Statistics.* The table keeps the bytes of name storage, serialised form included, for keys
whose count is above zero.
That figure follows the counts at once, on whichever thread changes them, so
`json_total_memory_bytes` does not wait for the sweep.

Every key is interned, as in the module. Objects with unique keys grow the table, as they do in
the module.

## 6. Parsing, tapes and serialising

### 6.1 Parse

Three steps, none recursive. The explicit stack starts in the caller's frame and moves to the
heap when it has to. The parser is hand-written (Q3).

1. **Tokenise** the input onto a tape: 8-byte entries plus a string arena, with no allocation
   per value. Object keys are interned as they are read, so the tape holds key IDs. The same
   scan sets the no-escape flag and, for an ordinary client, validates UTF-8 (B1). The string
   scan steps eight bytes at a time with word arithmetic. No SIMD instructions are used.
   Numbers are classified here (§5.4).
2. **Close each container.**
   - Duplicate keys are resolved as the module does (`document.h:2442-2450`): the later value
     takes the earlier member's position.
   - An array whose elements are all of one typed-array kind is marked with that kind, its
     width and, for decimals, its scale (§5.3).
   - The count, the exact packed size, and the bytes the emitter requests if the subtree goes
     to the spine are written back to the opening entry.
3. **Emit.** The tape now says, for every subtree, whether it fits the caps. The emitter writes
   the final encoding top-down with exact-size allocations.

**The key cache.** Looking a key up in the table is the largest per-key cost of parsing. The
parser remembers, for each nesting position, the keys of the last object it parsed there (256
positions, up to 256 keys per object). When it reads member i it first compares the incoming
key with the remembered i-th key and reuses its ID on a match. The cache holds no reference:
an ID it remembers is checked before it is used. On the prototype corpus the prediction was
right for all but 160 of 43 million keys and parse time on the small records fell by 15% to
17% (*prototype, superseded*).

**Limits.** A JSON text sent by a client is at most 2²⁹ bytes (512 MB). A longer text is
refused with the document size error. A document can grow past that through later commands,
so a text on a trusted path (§9) may be up to 2³² − 16 bytes, provided its parsed form stays
within 2³¹ entries, which is about 715 million values. The parser checks this before it
starts, by counting the `[`, `{`, `,` and `:` bytes of the text. Bytes inside strings are
counted too, so the check errs on the side of refusing. A single string is at most 2³² − 16
bytes and a single array at most 2³² − 16 elements. `JSON.STRAPPEND`, `JSON.ARRAPPEND` and
`JSON.ARRINSERT` refuse to grow one past that, with the document size error. Nothing stops a
document from passing the trusted bounds through many commands. Such a document is saved,
and the next load refuses it and logs that it is larger than this server loads.

**Measured in the server.** Whole-document `JSON.SET` takes 1.76, 2.58 and 3.60 µs for the
session, profile and order records, against 4.05, 6.27 and 8.96 µs in the module. For the
100,000-record array it takes 43.6 ms against 90.8 ms.

*History.* The prototype parsed the three records in 376, 709 and 1,138 ns at engine level on
a Mac, 1.4 to 2.2 times the speed of stock RapidJSON's DOM parse in the same harness
(*prototype, superseded*).

### 6.2 Tapes

A tape is a flat array of 8-byte entries plus an arena of string bytes. A parsed or built value
(`jsonVal`) is a tape that holds one complete value. Values reach a document only through a
tape: the tokeniser and the document walker write one, and the emitter reads it.

Tapes come from a pool of at most four. A user takes one and returns it. A tape whose buffers
grew past 8,192 entries or 64 KB of arena has them released when it returns to the pool. More
than one tape is in use when:

- A write holds its parsed value on one tape while it re-emits a node through another (§8).
- A module thread holding the GIL runs a JSON command.

RDB loading reads the whole text of a document before it starts to parse, for both encoding
versions (§9). `JSON.MSET` keeps, for each triple, the parsed document of a root path or the
planned change of a sub-path.

### 6.3 Key references

One rule, with no exceptions:

- **Every key entry on a tape holds one reference, and every key occurrence in a node holds
  one.**
- Interning during a parse takes the tape's reference.
- Walking an existing node onto a tape, or copying a span from one tape to another, takes a new
  reference for each key copied. The source keeps its own.
- Writing a key into a node from anywhere else takes a new reference too. That is the name of
  an inserted member, which comes from the path. The path keeps the reference it holds (§7).
- Emitting moves a tape entry's reference into the node. A tape entry is emitted at most once.
- Removing a byte range from a packed node releases the references of the keys inside it.
- Discarding a tape releases the references of entries that were not emitted.
- Freeing a node releases the node's references.

A consequence for writes: a value to write arrives as a tape, and writing it emits a copy. The
tape keeps its key references and the document gets its own. The tape a value was parsed onto
is emitted directly only when the value becomes the whole document.

Freeing a document walks it to release its keys. `FLUSHALL SYNC` of 500,010 keys takes 0.195 s
against the module's 0.729 s (measured).

### 6.4 Serialise

The serialiser is not recursive. All of its state lives in the caller's frame and it only
reads the document and the key table, so forkless save runs it on a save thread.

- **Two walkers.** Compact output of a packed value has a loop of its own that keeps the read
  and write positions in registers and checks the output buffer once per value. Spine
  containers, and any value when a format is given, go through the general walker.
- **Member names.** A member is written as one copy of its key's serialised form `"name":`
  from the key table (§5.5) followed by its value, with one buffer check for the member. The
  small helpers on this path are always inlined (`JSON_INLINE`), because gcc's inliner gave up
  on some of them inside the serialiser's loops, and a call there cost more than the helper.
  These three changes, in `2c17a3a01`, made serialising 10% to 25% faster on the record and
  number shapes. A profile on an x86-64 runner of a whole-document read of the record array
  had shown time in the member loop and in an out-of-line copy helper.
- **Reply.** Output starts in a stack buffer of 1,024 bytes. If it does not fit, the buffer
  moves to the heap. For a large value the command asks the engine for a size estimate
  (`jsonSerializeSizeHint`) and reserves the buffer once (`jsonBufReserve`). The reply is then
  one `ValkeyModule_ReplyWithStringBuffer`. The module API has no way to send a bulk string in
  pieces, so a large reply is built whole and copied once into the client's buffers, as in the
  module today.
- `INDENT`, `NEWLINE` and `SPACE` are format options that reproduce the module's output.
  `NOESCAPE` is accepted and ignored, as in the module (`json.cc:405-406`).
- **Formatted replies are bounded (C14).** The three format strings are written once or more
  per value, so long ones multiply the size of a document. When together they are longer than
  64 bytes, the command first computes an upper bound of the formatted text. Above 512 MB, the
  default maximum size of a bulk string, it replies `LIMIT Formatted JSON reply size limit is
  exceeded` and builds nothing. The external module builds whatever is asked for.
- **Other outputs:** RDB and AOF text, the digest (a separate non-recursive walk in the glue),
  `JSON.RESP`, and a tape (for re-emit).

**Measured in the server.** Whole-document `JSON.GET` takes 0.61, 0.84 and 1.05 µs for the
session, profile and order records, against 1.90, 2.52 and 3.23 µs in the module. With
`INDENT`, `NEWLINE` and `SPACE` on the profile record it takes 1.58 µs against 4.18 µs. For
the 100,000-record array it takes 13.2 ms against 14.4 ms. These are run c3, before
`2c17a3a01`. With the final code on Graviton the three records take 0.52, 0.71 and 0.88 µs
against 1.61, 2.30 and 2.94 µs, and the record array 11.2 ms against 14.1 ms (1.26×). On x86-64
hardware the record array was 0.97× the module's speed before the change, the only operation
slower than the module on any platform, and is 1.03× after it (10.55 ms against 10.91 ms). The
before and after runs on x86-64 were on different processors, so that comparison is
indicative (§15).

*History.* The prototype serialised the three records in 117, 306 and 549 ns at engine level,
2.7 to 5.0 times the speed of stock RapidJSON's writer (*prototype, superseded*).

### 6.5 Number printing and parsing (Q4)

`json_num.c` holds a number printer and a number parser written for this module. Neither
allocates, touches global mutable state or depends on the locale, and both are safe on any
thread. `strtod` and `fpconv` are not used.

- **Printing** uses Schubfach (Raffaello Giulietti, "The Schubfach way to render doubles",
  2020, published). The result is the shortest digit string that reads back as the same double
  and, among those, the closest to it: the digits of ECMAScript's `Number::toString` and of
  Python's `repr()`. `jsonDoublePrint` lays the digits out as the canonical text of §5.4.
- **Parsing** takes the first 19 significant digits as an integer and a decimal exponent, then
  tries in order: Clinger's fast path (one exact multiplication or division of doubles when
  the integer is at most 2⁵³ and the exponent is within ±22), a 64 by 128-bit multiplication
  by a table entry for the power of ten in the manner of Eisel-Lemire, and an exact fallback
  in decimal arithmetic on up to 800 digits. The result is correctly rounded, round-half-even.
- **The table** of 667 powers of ten as 128-bit integers (10,672 bytes) is generated by
  `tests/num/gen_pow10.py`, which also re-proves in exact arithmetic the bounds the code
  depends on.

Measured cost: 26 to 35 ns per number. Tested against Python: 33.3 million prints, 16.0
million parses, and 6.0 million mantissa and exponent pairs, with 0 failures (§11).

## 7. Paths

This section is a summary. The external module's selector is the specification, and
`json_path.c` is a port of its lexer and parser, function by function.

- A path compiles to a flat array of steps linked by index: member, index, slice, wildcard,
  union, recursive descent, filter. Legacy and `$` syntax compile to the same steps and differ
  only in reply shaping, as in the module.
- **A malformed path compiles.** The module parses a path while it evaluates it and stops
  reading a branch as soon as the branch has no value left, so whether a malformed path is an
  error depends on the document: `$.a[` is an error when the document has a member `a` and
  matches nothing when it has not. The compiler therefore ends the program with an error step,
  and the evaluator reports the error exactly when the module does. The only compile failure
  is a query above the size limit. A path ends at its first NUL byte, as in the module.
- A member step is resolved to a key ID when the name is in the key table. A name that is not
  there matches nothing. It is interned only when a write inserts it. A compiled path holds a
  reference on each ID it resolved.
- **Fast path.** A path of member and index steps only matches at most one value and resolves
  with no allocation. A result of up to four matches lives inside the result struct, so the
  single-match command allocates nothing.
- **The compiled-path cache.** Clients send the same few paths over and over, and compiling
  the path was a quarter of a single-path read in the first in-server profile. Compiled paths
  of up to 128 bytes are kept in a cache of 256 entries keyed by the path bytes. A repeated
  path costs 14 to 20 ns to compile, evaluate and release, with no allocation (measured. The
  notes do not record on which host). An entry that is handed out is not handed out again until it comes back, so
  two live compilations of one string are two entries. A new short path replaces the entry
  least recently used. A longer path, or any path when every entry is out, is compiled on the
  heap. An entry is close to 1 KB.
- **Cache entries hold key references.** That is what keeps their key IDs right for the next
  use. The side effect is on one statistic: a member name used by a cached path stays in the
  key table, and stays counted in `json_total_memory_bytes` (16 bytes per name), after the
  last document with that name is gone, until the entry is replaced.
- **Limits taken from the module**, each with the module's error string and kept as counters
  with the module's arithmetic, so the same inputs are rejected:
  - Document nesting: `json.max-path-limit` (128), checked on parse and on a sub-path write
    from an ordinary client (`LIMIT Document path nesting limit is exceeded`). A load and a
    trusted command are held to the load depth bound in its place (B9, §9).
  - Evaluator nesting: 200 (`LIMIT Parser recursion depth is exceeded`). The module needs it
    to bound recursion.
  - Recursive-descent tokens: 20. Query size: 128 KB.
- **Two bounds are the evaluator's own**, where the module has none. Both came out of the
  review (§11.1).
  - *Descent depth (C16).* A recursive descent goes at most 4,096 levels below the value it
    starts from (`JSON_PATH_MAX_DESCENT_DEPTH`), then fails with `LIMIT Parser recursion depth
    is exceeded`. The module recurses once per level there, outside its recursion counter, and
    the process dies of stack overflow between 5,000 and 20,000 levels in the reference build.
    A descent that starts further down a deep document is within the bound.
  - *Repeat work (C13).* A union that names one element more than once (`[0,0]`,
    `['a','a']`) makes a read return that element's matches once per mention, as in the
    module. A chain of such unions doubles the work at every step. The work a read spends on
    such repeats is bounded at 100,000 values plus 8 per value in the document
    (`JSON_PATH_REPEAT_FLOOR`, `JSON_PATH_REPEAT_PER_VALUE`). Past that the read fails with
    `LIMIT Total number of repeated matches in the query result exceeds the limit`
    (`JSON_PATH_ERR_TOO_MANY_REPEATS`), which is a syntax-class error and so fails the command
    with a `$` path too. Ten chained `[0,0]` steps still return their 1,024 matches. Twenty-two
    fail in 1.6 ms, where the module takes 1.07 s and builds a 209 MB reply. The update, insert
    and delete modes skip a repeated mention where it occurs, so a write is never refused and
    never sees a value twice.
- **Nested matches share one locator.** A match whose locator extends the locator of the match
  before it reuses those steps in the result's arena instead of copying them. On a document N
  levels deep a descent that matches every level stores N steps, not N²/2. When a second
  recursive descent only repeats matches the first one found, their locators go back to the
  arena, so repeats cost time and no memory. A crafted `RESTORE` payload of 20,000 levels made
  one query use 1.6 GB before this change and the two depth bounds.
- **Filters.** A `$...` path used as a filter operand has the same value for every element the
  filter tests, so it is evaluated once per evaluation and not once per element. A number in a
  filter is read by the engine's number parser (§6.5), which is correctly rounded and linear
  in the length of the text.
- **Not recursive.** The evaluator runs with an explicit stack of frames, one per fork in
  progress. The stack is bounded by the evaluator nesting limit plus the depth of the document
  under a recursive descent, which is at most 4,096.
- **Not tightly bounded in time:** nested recursive descents such as `$..*..*`. They are
  bounded in memory. One takes 396 ms on a document 2,000 levels deep with 8,000 values.
- **Result order is selector order**, the order in which the path produced the matches:
  `$[2,0]` yields element 2 first. Writes drop a value matched twice, keeping the first
  occurrence. Reads keep duplicates, except after a recursive descent. This is the module's
  behaviour (`selector.cc:2420-2432`, `:990`).
- **Modes**: read, update, insert-or-update, and delete, mapped one to one onto the module's
  selector entry points (`src/json/selector.h:100-144`). A key is added to an object only as
  the last step of the path. Missing parents are not created. Nothing is inserted into an
  array. A new name is literal (B5).
- **Slices** are bounded: a step or index that wraps or never returns in the module selects
  the first element or is out of range here (C5, C6).

Measured in the server on the profile record: `JSON.GET $.address.city` 0.52 µs against
1.77 µs, three paths in one `JSON.GET` 0.88 µs against 3.63 µs, `JSON.GET $..price` on the
order record 1.48 µs against 12.73 µs, and a filter `$.items[?(@.qty>4)].sku` 0.99 µs against
3.97 µs. On the 100,000-record array `$..price` takes 17.5 ms against 153.1 ms.

## 8. Writes

### 8.1 Matches

- A match carries a **locator**: the child steps from the root, plus its position in selector
  order. In an array a step is the element index. In an object it is the member's key ID. An
  insert is the parent's locator plus the new name.
- **Applicable.** Each command applies to some kinds of value. A match of another kind is not
  an error. With a `$` path it replies null in its position. With a legacy path it is skipped,
  and the command errors only if no match is applicable, as in the module (`dom.cc:556-562`).
- A legacy path replies with one result: the first or the last applicable match in selector
  order, per command, as in the module.
- **Every match is resolved against the document as it was before the command** (B4). If the
  command removes or replaces a value, a match inside that value has its effect dropped: the
  outermost wins. Commands that only add or edit in place apply to every match. Replies are
  computed from the pre-command document for every match (Q1, Q2).

### 8.2 One write, step by step

1. **Open the key.** Opening expires a key whose time has passed and fires `expired`, so it
   happens before anything else is held.
2. **Check**: resolve the path against the document as it is, parse the new value, test every
   limit, and compute every reply value. A new member name that the path adds is JSON text as
   much as the value is, so for an ordinary client it has to be valid UTF-8 (C15). A failure
   here leaves the document untouched (B6). On a trusted path the limits are replaced by the
   rule of §9.
3. **Apply**, in an order in which no change invalidates a locator still to be used: deeper
   before shallower, and within one array higher indexes first. This step does not fail.
4. **Replicate and notify**, in exactly the cases the module does (§2.4).
5. **Reply**, from the values computed in step 2.

No tape or cursor is in use after step 3. A keyspace callback is able to call back into the
engine through the shared API, and by then the document is final. A callback that changes the
keyspace is unsupported, as core's documentation says.

### 8.3 Three ways to change a packed node

A change names a value by its locator. The locator is followed through spine containers, slot
by slot, to the container the change is made in. A spine `ARRAY`, `OBJECT` or `TARRAY` is
changed directly: a slot is written, elements are moved, or the node is grown or rebuilt. A
container inside a packed node is changed with the cheapest of three primitives.

| Primitive | Used when | What it does |
|---|---|---|
| In place | The new bytes are as long as the old ones: a bool, a number that fits the existing width, a string of the same length | Overwrites the bytes |
| Splice | The size changes, every enclosing size field holds the new size, and the node stays within the caps | Replaces the byte range, moves the rest of the node, and patches the size of every enclosing container |
| Re-emit | A size field has to widen, or the node outgrows the caps | Copies the node to a tape with the change applied and emits it again. The emitter decides its new form, so there is no separate conversion code |

Splice is implemented for every edit that replaces one byte range of a packed node with
another: set a value, append to or trim an array or a string, insert or delete a member or an
element. All of it lives in section 11 of `json_engine.c`, the only file that writes packed
bytes.

**Measured in the server**, whole command, on the 596-byte profile record:

| Command | Primitive on a packed record | Module, µs | Built-in, µs | Speed-up |
|---|---|---:|---:|---:|
| `JSON.SET $.age` | In place | 1.57 | 0.62 | 2.53× |
| `JSON.NUMINCRBY $.age 1` | In place | 1.69 | 0.64 | 2.64× |
| `JSON.TOGGLE $.active` | In place | 1.21 | 0.58 | 2.09× |
| `JSON.SET $.name`, length changes | Splice | 1.55 | 0.85 | 1.82× |
| `JSON.STRAPPEND $.name` | Splice | 1.42 | 0.90 | 1.58× |
| `JSON.ARRAPPEND $.tags` | Splice | 1.73 | 0.95 | 1.82× |
| `JSON.ARRINSERT $.tags 0` | Splice | 1.53 | 0.97 | 1.58× |
| `JSON.SET $.extra`, a new member | Splice | 2.03 | 0.83 | 2.45× |
| `JSON.DEL $.extra` | Splice | 1.37 | 0.50 | 2.74× |

The primitive column is what the rules above select for a record of this size. The benchmark
did not record the primitive per call. Inside the 100,000-record array, where the record is a
packed leaf under a spine `ARRAY`, `NUMINCRBY $[n].qty` takes 0.73 µs against 1.68 µs and
`JSON.SET $[n].name` takes 0.90 µs against 1.50 µs.

**Counts by primitive.** The engine counts changes by primitive
(`json_engine_stats.h`). The working notes do not record the counts of the final test run. A
quick run of the change test on the phase 3 engine, across five cap settings and two builds,
recorded 248,980 changes: 10,710 in place (4%), 48,338 splice (19%), 8,900 re-emit (4%) and
181,032 on spine slots (73%). Three of the five cap settings (64/4, 16/1 and 0/0) leave little
or nothing packed, so the slot share in that run is higher than under the default caps.

*History.* The engine-level cost of each primitive as built was not recorded. The prototype
measured in place at 6 to 19 ns, splice at 71 to 254 ns and re-emit at 334 to 1,600 ns on
documents up to 3.8 KB, and splice fell back to re-emit for 70 of 100,000 profile edits
(*prototype, superseded*). Those figures are why splice exists: re-emit alone made a
size-changing write on a 4 KB document slower than the module's whole command.

### 8.4 The size limit and `JSON.MSET`

**Size limit.** `json.max-document-size` is checked in step 2 as an estimate: the document's
tracked size, plus the bytes the write adds, minus the bytes of what it removes. It ignores
allocator rounding and the growth of a node that is re-shaped into spine form. It is not
applied on a trusted path (§9). The module skips it for replicated commands too (`dom.cc:18`).

**`JSON.MSET`** is all or nothing across its keys, as in the module, which validates every
triple against the keyspace as it was before the command (`json.cc:288-368`).

1. Open every key.
2. Run the check step for every triple, each against the pre-command document. A new key with a
   non-root path is an error, as in the module (`json.cc:331-335`).
3. Run the apply step for every triple.
4. Replicate, and fire one `json.mset` event per triple in argument order, as the module does.
   Then reply.

**A key named more than once** is the one case where a later triple depends on an earlier one.
Its triples are run in order on a staged document: a copy of the key's current value, or the
first triple's value if that triple is a root path. Each is a complete check and apply against
the staged document. If one fails, the staged document is freed and the command returns the
error a standalone `JSON.SET` gives. Otherwise the staged value replaces the key's value in
step 3. The copy costs O(document) on the main thread, which the module does not pay. It is
the one exception to the goal in §4, and only this case pays it. When the triples do not
conflict the result is the module's. When they do, the module asserts (§2.5).

Measured: `JSON.MSET` of five keys with a `$.age` path takes 2.98 µs against 10.00 µs.

## 9. Persistence and replication

The data type keeps the module's name, `ReJSON-RL`, and its encoding version, 3. No part of
the stored or transmitted form changes.

**RDB and DUMP.** The save callback serialises the document to compact JSON text and writes it
as one string, exactly what the external module writes. The load callback parses it. Because
number text is exact (D2), a document saved by either implementation loads in the other with
identical content. This was checked both ways with the real module (§11).

- Encoding version 0, the module's older node-by-node form, is still read. The loader converts
  it to JSON text without recursion and parses that. A double in a version 0 payload is
  written with `%.17g`, as the external module does. Any other version is refused with a log
  line.
- Loading goes through the parser, so a payload is held to the same structural invariants as
  a command. It is a trusted path: UTF-8 and the size limit are not checked, and a number out
  of double range is kept as text.
- **Load depth bound (B9).** A payload is accepted up to 1,024 levels of nesting or
  `json.max-path-limit`, whichever is larger (`jsonLoadDepthLimit`). A stored document is
  allowed to be deeper than the limit, because the limit is able to drop after the document
  was written. The depth is still bounded, because a payload also comes from `RESTORE` and the
  cost of some commands grows with depth. A deeper payload fails the load: `RESTORE` replies
  `ERR Bad data format`, and an RDB file with such a document does not load until the limit is
  raised. Both encoding versions are held to the bound.
- Saving builds the whole text before writing it, as the module does. This is a document-sized
  buffer in the saving process or thread. A chunked form needs a new encoding version that the
  external module does not read, so it is not done.
- A binary RDB form is not in this design, because the external module does not read it. With
  the text form, loading 500,010 keys takes 2.020 s against the module's 3.097 s, and `SAVE`
  takes 1.973 s against 2.469 s (measured). The RDB files are 211,890,281 and 212,273,465
  bytes.

**AOF rewrite** emits `JSON.SET key . <text>`, as the module does.

**Replication** is verbatim, as today. The directed command suite compares the replication
stream of every step with the real module's.

**The rule for trusted commands.** A command that arrives from a primary or from an AOF file
(`VALKEYMODULE_CTX_FLAGS_REPLICATED` or `…_LOADING`) has already been accepted by the server
that stored the data. A replica or an AOF load must end up with what the primary stored,
whatever implementation or configuration the primary had. So:

- No client-side check is applied to it: not the nesting limit, not the size limit, not the
  512 MB limit on one JSON text (§6.1), not UTF-8 validation (B1, C15), not the number range
  (B2).
- Its JSON arguments end at their first NUL byte, because the external module reads an
  argument as a C string and that is what a module primary stored for the same command (C1).
- One bound does apply: the load depth bound. A replica must never hold a document it is
  unable to load again, so a trusted command that nests a document past the bound is refused.
  The depth an array command adds is counted exactly for this.
- The command asks whether it is trusted only when a check fails, which keeps the question
  off the path of an ordinary command.

Before the review, trusted commands skipped only B1 and B2. Two defects followed and are
fixed: an AOF rewrite lost a document nested deeper than `json.max-path-limit`, because
loading the AOF replays `JSON.SET` as a command, and a replicated `JSON.ARRAPPEND` nested a
document one level past the load bound. A later review of the whole implementation found a
third: the parser held trusted text to the 512 MB client limit, so a document that had grown
past it was saved and then refused on the next load. Trusted text now has the larger bound of
§6.1.

**Forkless save.** The built-in sets `VALKEYMODULE_OPTIONS_HANDLE_FORKLESS`, which the external
module does not. JSON data is therefore eligible for forkless save, with the save callback
running on a save thread. The serialiser and the key table are built for that (§5.5, §6.4).
§2.2 records what happens to a write while a save holds its key. Forkless save of JSON data
was exercised by the test suites only.

**Mixed versions.** The JSON data in an RDB file, a `DUMP` payload and the command stream is
understood in both directions by a server with the built-in and a server with the external
module. JSON adds no obstacle of its own. Whether an older server accepts a newer file is
decided by the core RDB version.

Mixed-version replication was run in both directions on loopback (measured):

| Check | External-module primary, built-in replica | Built-in primary, external-module replica |
|---|---|---|
| Full sync | Identical documents and digests | Identical documents and digests |
| A stream of 690 writes | 25 keys diverge | 26 keys diverge |
| Cause of every divergence | A listed change (B3, B4, B5, B6) or a module defect | The same |
| Text the module accepts and the built-in refuses from clients: invalid UTF-8, out-of-range numbers, NUL bytes | No divergence | Does not arise: the built-in primary refuses it |
| AOF file written by one and loaded by the other, compared after a rewrite | Agree | Agree, except one 129-level document that the module refuses |

What still differs between the two implementations is the direct result of a listed change:

- A command that produces different data: computed numbers (B3, C4), nested matches (B4),
  names containing `~` or `/` (B5), and a multi-match write that fails part-way (B6). Until
  every node runs the same implementation, a primary and its replica diverge on these. That
  is the 25 and 26 keys above.
- A document nested deeper than `json.max-path-limit`, which the built-in holds after the
  limit is lowered (B9), does not load in the external module. That is the 129-level document
  above.

## 10. Packaging and integration

The engine ships as a built-in module, following the Lua module in `unstable`.

| Piece | As built |
|---|---|
| Sources | `src/modules/json/`: five C files and seven headers, built into `libvalkeyjson.a` and linked whole into `valkey-server`. 16,169 lines of C in 12 files. Committed locally as `5a49c3386`, `1ae595934` and `2c17a3a01`, signed off, not pushed to valkey-io. The module builds with no warnings with gcc and clang. Every touched C file is clean under clang-format 18.1.8 |
| Core changes | `src/Makefile`, `src/CMakeLists.txt`, `CMakeLists.txt`, `src/server.c`, `src/config.c`, `src/server.h`, `valkey.conf`, `README.md`: 61 lines added and 2 changed |
| Entry points | `ValkeyModule_OnLoad_json` and `ValkeyModule_OnUnload_json`, resolved by name by `moduleLoadStatic("json", …)`. The module registers itself as `json`, version 20000 (2.0.0. The external module is 1.x) |
| Startup order | `main` calls `moduleInitModulesSystemLast`, then loads `json` if `json-enabled` is `yes`, then loads the configured modules (`moduleLoadFromQueue`). A module that looks for `json` while it loads finds it. A failure to load `json` stops the server with a panic |
| Build option | `BUILD_JSON`, on by default. `make BUILD_JSON=no`, or `-DBUILD_JSON=OFF` with CMake, leaves the module out of the binary. With it in, the server is compiled with `JSON_ENABLED` |
| Platforms | 64-bit and 32-bit targets of either byte order, so every build upstream CI makes, the 32-bit and s390x builds included, has JSON. On a 32-bit target a key's value points to an 8-byte heap cell that holds the document word (§5.1). On 32-bit x86 the module's Makefile and CMake file add `-msse2 -mfpmath=sse`: with x87 registers a double keeps excess precision, and 9007199254740993 compared equal to 9007199254740992.0 there and on no other target. The port found two real bugs: the key hash collided on big-endian (§5.5), and a size computed as a count times an element size, or by doubling a buffer, wrapped a 32-bit `size_t` once it passed 4 GB. Such sizes are now computed in 64 bits and checked. Tested on 32-bit x86 on a GitHub runner on real hardware (1,301 tests, active defrag included), on s390x on a GitHub runner under qemu (all JSON tests except the timing-based active defrag ones), and with the engine suites in i386 and s390x containers (§11). The test framework still skips every test tagged `json` when the server binary has no module (`server_has_json` in `tests/support/util.tcl`), for `BUILD_JSON=no` |
| `json-enabled` | A boolean server configuration, default `yes`, immutable at run time (D3). With `no` the server has no `JSON.*` command and no module `json`, and `loadmodule` loads the external module. With `yes` and a `loadmodule` line for the external module, the server logs `module name is busy` and aborts during startup (B10). `MODULE LOAD` of the external module at run time fails for the same reason. The setting has no effect on a server built without the module. A server that starts without the module does not accept `json.` settings, so they come out of the configuration together with `json-enabled no` |
| Data type | `ValkeyModule_CreateDataType("ReJSON-RL", 3, …)` with `rdb_load`, `rdb_save`, `aof_rewrite`, `free`, `digest`, `defrag`, `mem_usage2`, `free_effort2` and `copy2`. No `unlink2` is registered |
| Options | `VALKEYMODULE_OPTIONS_HANDLE_IO_ERRORS`, `…_HANDLE_REPL_ASYNC_LOAD`, `…_HANDLE_ATOMIC_SLOT_MIGRATION` and `…_HANDLE_FORKLESS`. The last two are new (§2.2) |
| Commands | The same 24 registrations, with the module's names, flags, key positions, arity, ACL categories and key specifications, and `JSON.DEBUG` with the subcommands kept in B8 |
| Configs | Four numeric module configs: `json.max-document-size` (memory value, default 0 = no limit), `json.max-path-limit` (default 128), `json.max-packed-bytes` (default 4096, 0 to 65535) and `json.max-packed-entries` (default 128, 0 to 65535). All four are settable at run time. The two caps apply to nodes built afterwards |
| ACL, INFO, events | `ValkeyModule_AddACLCategory("json")`, `ValkeyModule_RegisterInfoFunc` with section `core_metrics` and the fields `total_memory_bytes` and `num_documents`, `ValkeyModule_NotifyKeyspaceEvent` with the module's names |
| Shared API | `ValkeyModule_ExportSharedAPI("JSON_GetValue", …)` with the module's signature and results: an error for a legacy path with no match, success with `[]` for a `$` path with no match |
| Plain C API | The five functions of `json_api.h`, exported by the server binary with the external module's names, signatures, results and memory rules. Tested through a test module (§11) |
| Allocator | The engine takes its allocator from the glue, which passes `ValkeyModule_Alloc`, `Realloc`, `Free` and `MallocUsableSize` |
| Key table sweep | A handler on the server's cron event calls `jsonKeyTableSweep` |

**Lazy free.** `free_effort2` returns 1 for a packed root and the number of heap nodes for a
spine document, in O(1) from the root's node count. The server compares it with its lazy-free
threshold and frees a large document on its background thread. The free callback is safe
there (§5.5), and the statistics it touches are atomic. Measured effect: `JSON.DEL` of the
100,000-record key takes 18.65 µs on the main thread against 28,020 µs in the module, which
frees it on the main thread and blocks every client for 28 ms. The side effect is C11:
`json_total_memory_bytes` and `json_num_documents` settle when the background free runs. The
benchmark needs two cores per server for this row. With one core the background job shares the
main thread's core and 9 ms of its time lands in the `JSON.DEL` figure.

**Defrag.** The data type's defrag callback offers every allocation of a document to
`ValkeyModule_DefragAlloc` once. A document whose root is not a spine container is a single
allocation. A spine document is done in steps counted by the cursor the module API provides
(`ValkeyModule_DefragCursorGet`, `…Set`, `…ShouldStop`): the high half of the cursor is the
index of a child of the root plus one, and the low half is an index inside that child when the
child is itself a spine container. Anything below that second level moves as one step. Core
offers a cursor only for a value whose free effort exceeds `active-defrag-max-scan-fields`. A
smaller document is done in one call. The cursor is positional and commands run between steps,
so every step re-checks the root and its bounds. After a change some nodes are offered twice
and some not at all, which defragmentation tolerates. Key bytes live in the key table and are
not moved. This replaces the module's whole-document copy. It was exercised on Linux with
jemalloc: four active-defrag tests of JSON documents pass (§11).

Files, as built:

| File | Contents |
|---|---|
| `json_module.c`, `json_module.h` | Entry point, data type callbacks (RDB, AOF, copy, digest, defrag, free), configuration, statistics, shared API. The only file that connects the engine to the server |
| `json_cmd.c` | The 24 commands: argument parsing, replies, replication and events. Also the five functions of `json_api.h` |
| `json_path.c`, `json_path.h` | Path compiler and evaluator, legacy and JSONPath syntax, and the compiled-path cache |
| `json_engine.c`, `json_engine.h`, `json_engine_format.h`, `json_engine_stats.h` | In-memory document format, key table, tapes, tokeniser, emitter, serialiser, every change to a document, and defrag |
| `json_num.c`, `json_num.h` | Shortest round-trip number printing and correctly rounded number parsing |

`json_engine.c`, `json_path.c` and `json_num.c` include no server header and take their
allocator from the caller, so they also build as standalone test programs.

One addition to the module API is useful and is not required: a way to send a bulk string in
pieces, so a large reply needs no whole-document buffer. A second one benefits every module:
removing the 16-byte wrapper the module API allocates for each module value, which is one of
the two cache misses in a single-path read (§2.2).

## 11. Testing

What exists now, and its results on 2026-11-14 and 2026-11-15. "The real module" is the external module at
`60cfc5c` running in a reference server.

| Suite | Size | Result |
|---|---|---|
| Engine against an independent Python model | 1,192,436 comparisons and 357,135 change steps, under five cap settings (4096/128, 64/4, 16/1, 0/0, 65535/65535) | 0 failures. Also under ASan and UBSan, and with gcc on Linux |
| Engine, deliberately planted bugs | 34 across four phases | 33 caught. The one miss led to a new check, and that bug is caught now |
| Number printer and parser against Python | 33.3 million prints, 16.0 million parses, 6.0 million mantissa and exponent pairs | 0 failures |
| Path evaluator against the real module | 36,205 document and path pairs | 0 failures |
| Commands, directed, against the real module | 3,354 steps, RESP2 and RESP3, replies with wire types, keyspace events, replication stream | 0 failures |
| Commands, random differential, against the real module | 12 seeds of 50,000 commands | No crash, no desync. 99 to 154 differences per seed outside B1 to B10, all in the classes of §2.5 |
| Persistence against the real module | 21 checks: reload, `DUMP` and `RESTORE` both ways, `COPY`, RDB both ways | The built-in passes all |
| Replication and AOF between the two implementations | Both directions on loopback: full sync, a stream of 690 writes, AOF both ways (`tests/cmd/test_repl.py`) | 0 unclassified divergences. 25 and 26 keys diverge, each on a listed change or a module defect (§9) |
| Plain C API through a test module | 67 calls on both implementations (`tests/cmd/test_api.py`, `tests/cmd/apimod`) | Identical results |
| The interfaces valkey-search uses, through a test module | 157 commands and 58 keyspace events on both implementations | Identical. valkey-search itself was not run (§2.4) |
| The external module's own pytest suite | 127 cases | 118 pass. Eight fail by design (B3 four, B4 one, B5 one, B8 two). One fails on the background free (C11) |
| New Tcl suite in the tree | 14 files, 13,059 lines: `tests/unit/type/json*.tcl`, `tests/integration/json-persistence.tcl`, `tests/integration/json-replication.tcl`, one RDB asset. 1,113 executions, including the tests added for the review fixes. Cases run under several cap settings | All pass on macOS (Tcl 9.0 and 8.6). On Linux (Tcl 8.6) 1,107 pass and none fail in the final run |
| Active defrag of JSON documents | 4 tests, Linux with jemalloc | All pass, also inside the final Linux run below |
| Valkey's full test suite with the module built in, Linux, final code | 200 test files, 7,445 tests | All pass. An earlier run had one failure, "CLIENT KILL close the client connection during bgsave - forkless", which also fails 3 of 3 on a server built without the module: the test waits 10 s for a save that sleeps 10 s |
| Final code on GitHub-hosted runners (`2c17a3a01`), four jobs | x86-64: engine, number, path and command suites against the external module, replication, Valkey's full test suite. 32-bit x86: the JSON tests and the core test files the change touches, 1,301 tests including active defrag, on real hardware. s390x under qemu: the path suite and the JSON tests, except the timing-based active defrag tests | All four jobs pass |
| Final code, engine suite on Linux arm64 (`Graviton host A`, gcc, ASan and UBSan) | Full form | 0 failures |
| 32-bit and big-endian port, engine suites in containers | i386 and s390x, quick form with UBSan, plus a byte-layout corpus of 4,998 documents | 0 failures. Three bugs planted for the port were each caught on the affected target |
| x86-64 under Rosetta emulation, committed tree before `2c17a3a01` (*superseded* by the runs on x86-64 hardware above) | 1,171 Tcl tests (the JSON files, minus the cases that need the external module, plus the two core test files the change touches), the 3,354-step directed comparison, the 36,205 path cases, and the engine and number suites with their ASan and UBSan builds (309,210 change steps) | All pass. The tree builds for x86-64 with no warnings. This exercised the x86-64 code the compiler generates, before x86 hardware was available |
| Valkey's full test suite with the module built in, macOS | 200 test files, 7,335 tests, run before the review fixes | 7,332 pass. Three fail: one cluster `del-node` test that fails 2 of 2 on pristine upstream at the same commit, and two replication-throttle tests that pass when rerun alone |
| The JSON tests on a server built without the module | 14 files | Every JSON block is skipped, 0 failures |
| Corrupt-payload fuzzer with JSON documents | Default duration | Pass |
| Long fuzzing, Linux arm64, gcc with ASan and UBSan, about 3 hours (2026-11-15) | Engine: 9 seeds, 7.0 million iterations. `RESTORE` of mutated payloads: 2.86 million. Paths: 87.5 million commands against an ASan and a UBSan server side by side | 0 sanitizer reports, 0 mismatches, 0 leaked keys at shutdown |
| Commands, random differential against the real module, 3 hours, 24 workers | 733 million commands, half RESP2 and half RESP3, 251 persistence runs | The built-in never crashed, the module crashed 472 times. Three new kinds of difference: two module defects, and a parser bug in the built-in that dropped leading spaces of quoted literals, now fixed (C17, C18) |
| Soak, release build with jemalloc, 170 minutes | 341.6 million operations at 34,000 per second, 42 `BGSAVE` (24 fork, 18 forkless), 5 `DEBUG RELOAD` | 0 unexpected errors. No memory trend after warm-up. p99 about 10 µs. The document count equalled `DBSIZE` at every quiet point |
| Cluster tests: slot migration (classic and atomic), keyspace, failover, resharding | 23 tests in `tests/unit/cluster/json-*.tcl` | All pass, also under ASan, UBSan and valgrind |
| The JSON files against a cluster | 15 files, keys used together share a hash tag | 994 pass, 0 fail |
| RDB encoding 0, the external module's oldest format | `tests/unit/type/json-rdb-v0.tcl`: payloads built byte by byte, every node type, nesting at and beyond the load bound, over 100 malformed payloads | All valid payloads load, all malformed ones are refused, no crash |
| Final code (`2170143fc`) on GitHub-hosted x86-64 runners | Valkey's whole suite plain, under ASan and UBSan (gcc and clang, module instrumented), with TLS, and under valgrind. The JSON files with I/O threads, on Alpine (musl), on a cluster. 32-bit x86, s390x | Plain 7,495, ASan 7,442 and 7,437, UBSan 7,452 and 7,444, TLS 7,408: 0 failures. valgrind, I/O threads (1,117), musl (1,117), cluster (994 and 1,020), 32-bit and s390x pass |
| Line coverage of the module from all suites | gcov | 99.1% of lines, 99.4% of functions |
| Upgrade from Valkey 9.0.7 with valkey-json 1.1.0 and 1.0.3 | RDB, AOF with and without preamble, a replication stream of 21,197 commands, 101,698 keys | RDB data identical. AOF and stream identical except B3. Downgrade by file needs `rdb-version-check relaxed` on 9.0.7, because the core writes RDB version 81 |

Before 2026-11-15 the module's sub-make did not receive the server's sanitizer and coverage
flags, so sanitizer builds of the server ran an uninstrumented module. The Makefile and the
CMake target now pass them, and the sanitizer rows above include the module.

Supporting changes in the core tests, 35 lines by the write-up's count: JSON commands in the
fuzz traffic list and the `server_has_json` probe (`tests/support/util.tcl`), the skip of
tests tagged `json` on a server without the module (`tests/support/server.tcl`), two JSON
documents in the corrupt-payload fuzzer, and two introspection lists.

The Tcl tests that compare with the external module take the path of its shared library from
the environment variable `VALKEY_JSON_EXTERNAL_MODULE` only, and are skipped without it.

The development tests live under `src/modules/json/tests/{engine,path,num,cmd}`: 12,811 lines
of C and Python. They compare the engine with a Python model, and the path evaluator and the
commands with a server running the real module. They are not wired into `make test`. Upstream
is expected to want the engine tests in the unit test framework.

Both compilers build the module with zero warnings. Every touched C file is clean under
clang-format 18.1.8, the version CI pins.

Planned in earlier versions of this section and not done:

- Fuzzing for days rather than hours. libFuzzer and JSONTestSuite were not used.
- valkey-search itself against the built-in. The `valkey-fuzzer` command list.
- JSON commands during an async RDB load, and replication under load.
- A run on big-endian hardware. s390x ran under qemu, so the timing-based active defrag tests
  were not run there.
- A port of the external module's gtest unit cases. The Python model and the differential
  tests took their place.

### 11.1 Independent review

Two AI reviewers that had not seen the code before read it adversarially and attacked it.
Neither is a substitute for review by a person.

**Engine and number code.** No confirmed defect. Covered: fuzzing of parse, serialise, change,
copy, defrag and free under ASan and UBSan, a thread-sanitizer run of the key table with three
lazy-free threads and a save thread (2.1 million iterations), and nesting of 500,000 levels.

**Commands, paths and glue.** No memory-safety fault, leak or protocol desync in 6 million
stress commands (rounded) under ASan. Five confirmed defects, all fixed, each with a test:

| # | Finding | Fix | Listed as |
|---|---|---|---|
| 1 | A path with repeated union members forks exponentially: 1.07 s and 209 MB for one `JSON.GET`. The module has the same defect | A work bound relative to the document. The same query fails in 1.6 ms | C13, §7 |
| 2 | AOF rewrite lost a document nested deeper than `json.max-path-limit` | Commands from a primary or an AOF file skip the client limits | §9 |
| 3 | A crafted `RESTORE` payload of 20,000 levels made one query use 1.6 GB | Load accepts 1,024 levels or the limit, whichever is larger. Recursive descent stops 4,096 levels below its start. Nested matches share one locator | B9, C16, §7 |
| 4 | `JSON.DEBUG MAX-SIZE-KEY` let a key name inject protocol replies. The module has the same defect | Bulk string reply | C12 |
| 5 | Invalid UTF-8 entered through a new member name in a path | Validated for ordinary clients | C15 |

Two more came out of the same work. The review found that the module refused to compile on
32-bit and big-endian targets, which upstream CI builds. The module has since been ported to
them, and the port found two bugs of its own (§10). The Tcl tests written for the fixes found that a replicated `JSON.ARRAPPEND` nested a
document one level past the load bound, which is fixed (§9).

**Cost of the fixes.** Measured on the idle host against the build before the fixes, the
median cost over the 69 operations is 1.1%. The maximum is 9% on `JSON.MSET` (2.16 to 2.36 µs),
from validating member names (§15).

## 12. Milestones

The implementation work recorded here happened on 2026-11-14, and the port, the serialiser
change and the x86-64 runs on 2026-11-15. This document gives no effort estimate for what
remains.

| | Scope | State |
|---|---|---|
| M0a | A standalone prototype of the engine, and the external module measured on a local server, on one Mac with a synthetic corpus | Done 2026-11-14. Its measurements are superseded by §15 |
| M0b | The same measurements on Linux under jemalloc, with both implementations under one allocator. A Schubfach-class printer. yyjson's reader as a parser variant. Real documents. Contended reference counts | Done 2026-11-14 on two Graviton hosts with the engine inside the server. The printer is written and measured. A thread-sanitizer run of the key table is clean. Eight real-world files and a 50-connection run were measured. x86-64 was checked under emulation on 2026-11-14 and measured on GitHub-hosted runners on 2026-11-15 (§15). Not done: x86 hosts of the author's own, production documents, the yyjson variant (closed, Q3), timing of contended reference counts |
| M1 | The built-in module skeleton. Data type callbacks. Root `SET`, `GET`, `DEL`, `TYPE`. RDB, AOF, `DUMP`, forkless save. Packed nodes and spine nodes. Key table | Done 2026-11-14. External-module payloads load, and the external module loads ours. Memory gates met at server level. The key table ran under the thread sanitizer in the review (§11.1) |
| M2 | Path compiler and evaluator. Every read command | Done 2026-11-14 (read side complete by 11:40). Read gates met at server level |
| M3 | Writes: in place, splice, re-emit, every write command, `MSET`, `MERGE` | Done 2026-11-14 (all 24 commands running by 12:50). The module's suite passes except the nine cases of §2.3. Differential runs show only the classes of §2.5. Write gates met at server level |
| M4 | Typed arrays. Incremental defrag | Done 2026-11-14. Decimals, inline string arrays and the faster serialiser merged by 13:50. 18-digit decimals and literal arrays merged by 15:55. Suite clean under several cap settings. Defrag tests pass on Linux. Fragmentation after a write-heavy workload was not measured |
| M5 | Shared API, the plain C API, INFO, `JSON.DEBUG` | Done 2026-11-14. Both APIs are tested. The interfaces valkey-search uses were exercised by a test module with 0 differences. valkey-search itself was not run |
| M6 | SIMD scanning, fuzzing soak, `valkey-doc`, `valkey-fuzzer` | Documentation done 2026-11-14: the `valkey-doc` change is local commit `a7a40a1` on branch `json-builtin` of `~/workspace/valkey-doc` (16 files: the JSON topic page, ten command pages, the data types and quick start pages). Its link check passes. Its spell checker was not run because it needs a download. Not pushed. Two later local commits, `8d49e06` and `a56783c`, add the platform text for the port and C17/C18. The upstream pull-request text is in `json-m0/docs/UPSTREAM_PR.md` and `UPSTREAM_PR_BODY.md`. Not done: SIMD scanning, a fuzzing soak, the `valkey-fuzzer` command list |
| Review | Two independent AI reviews, their fixes and tests, mixed-version replication, the C API test, the no-JSON-build check, and a replicate benchmark | Done 2026-11-14 (§11.1, §15) |
| Port and x86-64 | 32-bit and big-endian targets, a faster serialiser, clang-format 18.1.8, and a CI workflow on the fork that benchmarks on x86-64 and tests on x86-64, 32-bit x86 and s390x | Done 2026-11-15 in `2c17a3a01`, with the workflow on the fork's branch `json-builtin-ci`. All 69 operations faster on x86-64 hardware, median 2.30×. All four CI jobs pass (§10, §11, §15) |
| Hardening | Cluster tests, the whole suite under ASan, UBSan, valgrind and TLS, I/O threads, musl, coverage, three-hour fuzzing and differential runs, a 170-minute soak, upgrade and downgrade against Valkey 9.0.7 | Done 2026-11-15 and 2026-11-16, final commit `2170143fc`. One parser bug found and fixed (C17, C18). Sanitizer and coverage builds now instrument the module. Every CI job passes (§11) |

The benchmark tables are run c3 on the first host. The replicate is run h2 and the code after
the review fixes is run h3, both on the second host. The final code is
`report-final-graviton.md` on Graviton and run 43694898886 on an x86-64 runner (§15).

What remains, in the order it reduces risk:

1. Review of the code by people. The code and tests were written by AI agents working to
   written interfaces, checked by tests against the real module and by two AI reviewers.
2. Running valkey-search itself against the built-in.
3. Fuzzing for days. Three hours per fuzzer ran clean (§11).
4. x86 measurements on dedicated hosts. The x86-64 figures come from shared 4-vCPU GitHub
   runners whose processor changes between runs. The author's own x86 hosts were unreachable.
5. A run on big-endian hardware. s390x ran under qemu only.
6. Opening the upstream proposals: the pull request on valkey-io/valkey, whose text is in
   `json-m0/docs/UPSTREAM_PR.md` and `UPSTREAM_PR_BODY.md`, and the `valkey-doc` change of
   commit `a7a40a1`. Nothing is pushed to valkey-io. The code reaches GitHub only as the
   fork's branch `json-builtin-ci`, which CI runs on. The `valkey-doc` change is local.

Production documents and the `valkey-fuzzer` command list also remain open.

## 13. Risks

| Risk | State and response |
|---|---|
| The x86-64 measurements are from shared virtual machines, and the corpus is synthetic plus eight real files | Open, narrowed. Two Graviton3 hosts agree: identical memory, and medians of 2.49× on the shared host and 2.64× on the idle one. On x86-64 GitHub-hosted runners with 4 vCPUs the final code has the same memory and is faster on all 69 operations, median 2.30×, minimum 1.03×. The runner's processor changes between runs, so a ratio near 1 is within its noise. Eight real-world files gave 6% to 67% of the module's memory. Documents dominated by long strings save less: 60% to 67%. Item 4 of §12 |
| The memory gate for small records has little margin at server level | The session and profile records are at 49% against a gate of 50% when the key name and keyspace entry are counted. Member-name sharing, string lengths and number formats in real workloads move the ratio. Sharing object shapes is the measured next step if the gate is missed: a further 6% to 8% in the prototype (§14) |
| Reading back an array of doubles was slower than the module | Closed. Decimals and the new printer put `JSON.GET` of the two embeddings at 2.48× and 2.57× the module's speed (§4) |
| A large array of 8 to 13-byte strings used 48% more memory than the module | Closed at parity. Arrays of 16-byte inline strings bring it to 100%. A gain on this shape needs variable-width storage, which is not built |
| A size cliff where a container crosses the entries cap: a 100-member object of small integers is 392 bytes packed and was 2,048 bytes as a spine object in the prototype | Open. The default cap of 128 is above anything in the gate corpus. It has not been tuned on real data |
| The built-in is in every server, so a defect in it reaches users who store no JSON | `json-enabled no` and `BUILD_JSON=no` remove it. On macOS, before the review fixes, Valkey's full suite passed with the module built in except three tests that do not involve it (§11). On Linux the full suite passes with the module built in, on x86-64 runners also under ASan, UBSan, valgrind and TLS with the module instrumented (§11). Since the port, 32-bit and big-endian servers carry the module too |
| 32-bit and big-endian builds | Closed. The module runs on 64-bit and 32-bit targets of either byte order (§10). The JSON tests pass on 32-bit x86 on real hardware and on s390x under qemu. The port found two bugs, both fixed: a key hash collision on big-endian and sizes that wrapped a 32-bit `size_t` past 4 GB. Big-endian hardware has not run it, and speed on these targets is not measured. Item 5 of §12 |
| The module's behaviour is the specification, and parts of it are undefined or unknown | Eighteen differences were found by testing and review (§2.5). Twelve random seeds of 50,000 commands, a three-hour differential run of 733 million commands and a mixed-version replication run show no other class. Real client traffic has not been replayed |
| The key table is shared across keys and touched by two background threads | Slots do not move and key bytes are immutable while referenced (§5.5). A thread-sanitizer run with three lazy-free threads and a save thread, 2.1 million iterations, found nothing (§11.1). The whole suite passes under ASan and UBSan with the module instrumented, and a 170-minute soak with lazy free and 42 saves showed no drift (§11) |
| Forkless save runs on JSON data for the first time | Exercised by the test suites and by 18 forkless saves under load in the soak, whose digests never changed (§11). Not timed |
| The write path has the most interacting rules: references, tapes, three primitives, all-or-nothing | One reference rule with no exceptions (§6.3). 357,135 change steps against the Python model with 0 failures, and 33 of 34 planted bugs caught |
| A bug in a write primitive corrupts a blob | Every write to packed bytes is in one file. Blobs are at most 65,535 bytes. The engine tests run under ASan and UBSan |
| No person has reviewed the code | Open. Two independent AI reviews found five defects, all fixed (§11.1). They do not replace review by a person. Item 1 of §12 |
| valkey-search itself has not been run against the built-in | Open. A test module that uses the same interfaces the same way found 0 differences over 157 commands and 58 keyspace events (§2.4). A test module is not valkey-search. Item 2 of §12 |
| A path costs more than its document justifies | Bounded for repeated unions, for descent depth and for formatted replies (C13, C14, C16). Nested recursive descents such as `$..*..*` are bounded in memory and not tightly in time: 396 ms on a document 2,000 levels deep with 8,000 values |
| Stored documents deeper than the load depth bound | A document deeper than 1,024 levels loads only while `json.max-path-limit` is at or above its depth (B9). An operator who lowers the limit below such a document is not able to reload it until the limit is raised again |
| One statistic is slightly off | A member name used by a cached path stays counted in `json_total_memory_bytes`, 16 bytes per name, after its documents are gone (§7) |
| Upstream prefers the module to stay in its own repository | The engine, path and number files include no server header. The test record has no build of the same sources as a loadable module |
| A hand-written parser and number code are new input-facing code | The Python model, the differential suite, the number tests of §11, fuzzing under ASan and UBSan in the review, the corrupt-payload fuzzer, and three hours each of engine, `RESTORE` and path fuzzing under ASan and UBSan with no report (§11). Not days |

## 14. Alternatives considered

| Alternative | Outcome |
|---|---|
| A native core data type | Not now (D1, Q6). It removes the module interface from every command, including the 16-byte wrapper per value, and allows replies to stream without a whole-document buffer. It changes `TYPE`, `MODULE LIST`, the key type other modules see, the statistics names and the config names, and it requires a patch to valkey-search and a new RDB type. The engine files do not change |
| Keep it an external module | Not chosen. Built-in is preferred because JSON is then present without a `loadmodule` line and is built and released with the server |
| Normalise numbers | Decided against (D2). It changes number text for every client and drops precision silently. Its speed argument was printing: with decimals, a number of up to 18 digits needs none |
| A per-array "fixed with k fraction digits" format for doubles | Designed, then replaced by decimals before it shipped (§5.4). A fixed-k array used 8 bytes per element where a decimal array of the same values uses 4 |
| Share whole object shapes across documents, storing one shape ID per object | Not built. Measured in the prototype: a further 6% to 8% memory on small records, 17% on the record array, and 4% to 9% of parse time (*prototype, superseded*). It needs a second table with transitions on every member insert and delete, and objects with dynamic keys make it grow without bound. It is the first thing to revisit if a memory gate is missed on real data |
| Keys-first object layout | Measured in the prototype and rejected (§5.2) |
| Lossless float32 storage for arrays of doubles | Not built. Measured in the prototype: halves a full-precision embedding to 7,168 bytes when every element qualifies (*prototype, superseded*) |
| Store a large array as runs of packed chunks | Not built. It was the planned remedy for the short-string regression. Arrays of 16-byte inline strings reached parity on the existing typed-array mechanism, with no new node kind and no index from element number to chunk. Chunks remain the route to a gain on that shape (12 bytes per element against the module's 16, derived) |
| Re-emit only, no splice | Measured in the prototype and rejected (§8.3) |
| Vendor yyjson as the tokeniser | Not evaluated (Q3). The hand-written parser met the parse gates. yyjson remains an option if upstream prefers less new input-facing code |
| Valkey's `fpconv` as the number printer | Rejected. It is not always shortest (§5.4) |
| An existing binary format for packed nodes | CBOR and MessagePack containers carry a count but no byte size, so a sibling is not skippable. BSON stores every key as text. PostgreSQL JSONB, MySQL's binary JSON and Oracle OSON sort keys and lose insertion order. SQLite's JSONB keeps order and splices in place, and keeps all keys and numbers as text. None has key IDs shared across documents (published survey) |
| A pointer tree with no packed encoding | Fails the small-record memory gate: one allocation per container and per long string remains |
| Interned keys that are never freed | Removes reference counts, the sweep and the key walk on free. A workload with ever-new keys then grows the table without bound |

## 15. What was measured, and how

**Hosts.** `Graviton host A` and `Graviton host B`, both c7g.16xlarge (64-core Graviton3), Amazon
Linux 2023, gcc 11.5, jemalloc 5.3. `Graviton host A` is shared with other jobs and `Graviton host B`
was idle. The tables in §3 to §9 are from `Graviton host A`. The replicate below is from
`Graviton host B`. The x86-64 runs used GitHub-hosted runners and are described in their own
paragraph below. On `Graviton host A` the disk was close to full, so everything ran from
`/dev/shm/json-bench/`, which is memory-backed.

**Servers.** The same Valkey source for both sides. The reference is built with
`BUILD_JSON=no` and loads the external module (the `valkey-json-pr95` checkout at `60cfc5c`,
built `-O3 -DNDEBUG`). The candidate has JSON built in. Both use jemalloc.

**Isolation.** Each server is pinned to two cores and the client to a third. Unix socket only.
No TCP listener was opened.

**Measurement.** Server-side microseconds per call from `INFO commandstats`, 200,000 calls for
most operations after a warm-up. Three rounds per side, interleaved. Figures are medians of
the three. These rounds use one client and one pipeline, with no I/O threads. A separate run
with 50 connections is below.

**Noise.** Across three rounds of one build the median spread per operation is 3.2% and the
worst is 12%. Core commands (`GET`, `SET`, `HGET`, `HSET`, `HINCRBY`, `STRLEN`) measured on
both servers are equal within run-to-run variation. The hash commands are bimodal on the
shared host on both sides.

**Corpus.** The gate corpus is synthetic and deterministic, from `json-m0/gen.py`: 100,000 documents each of three
record shapes (session, profile, order, averaging 307, 596 and 747 bytes of text), and one
document each of six large shapes (the record array, two embeddings, the integer array, the
string array, the 100-member object).

**Memory.** Bytes per key. For the three record shapes it is the growth of the server's
`used_memory` over 100,000 keys, so it includes the key name and the keyspace entry. For the
single large documents it is `MEMORY USAGE`, because the `used_memory` difference for one key
is polluted by the client query buffer.

**Results.** Memory: §4 and §5. Time per command: §4, §7 and §8, with all 76 rows (69 JSON
operations and 7 core commands) in `json-m0/bench/results/report-c3.md`. Over the 69 JSON
operations the median speed-up is 2.49×, the geometric mean is 2.63× with any single ratio
capped at 10, the minimum is 1.09×, and none is slower than the module. These are run c3.
The final code is measured below, on Graviton and on x86-64 hardware.

Save, load and free, 500,010 keys:

| | External module | Built-in |
|---|---:|---:|
| `SAVE`, seconds | 2.469 | 1.973 |
| Load (`DEBUG RELOAD NOSAVE`), seconds | 3.097 | 2.020 |
| `FLUSHALL SYNC`, seconds | 0.729 | 0.195 |
| RDB file, bytes | 212,273,465 | 211,890,281 |

**Raw results and tools.** `json-m0/bench/results/c3-*.json`, `json-m0/bench/bench.py` and
`report.py`. Compatibility tools: `json-m0/compat/README.md`. The results write-up is
`VALKEY_JSON_BUILTIN_RESULTS.md`, and its section 8 gives the commands to reproduce.

**Replicate on a second host.** The same three interleaved rounds on `Graviton host B`, with the
build from before the review fixes (run h2).

| | `Graviton host A` (shared) | `Graviton host B` (idle) |
|---|---:|---:|
| Memory, all nine shapes | as in §4 and §5 | identical byte counts |
| JSON operations faster than the module | 69 of 69 | 69 of 69 |
| Median speed-up | 2.49× | 2.64× |
| Minimum speed-up | 1.09× | 1.14× |
| `JSON.GET $.address.city`, µs | 0.52 | 0.39 |
| `HGET`, µs, module server and built-in server | 0.37 and 0.45 | 0.36 and 0.36 |
| `SAVE`, seconds, module and built-in | 2.469 and 1.973 | 2.464 and 1.941 |

On the idle host the core commands cost the same on both servers, which confirms that the
`HGET` difference seen on the shared host was noise. Raw results:
`json-m0/bench/results/h2-*.json` and `report-h2.md`.

**After the review fixes.** The final code, with the fixes of §11.1, was measured on
`Graviton host B` in three more rounds against the same module baseline (run h3): faster on 69 of
69, median 2.60×, minimum 1.13×, memory unchanged. Against the build before the fixes on the
same host, the median cost of the fixes is 1.1%. The maximum is 9% on `JSON.MSET` (2.16 to
2.36 µs), from validating member names. Raw results: `h3-cand-*.json` and `report-h3.md`.

**Final code on Graviton.** The code of `2c17a3a01`, with the port and the serialiser change,
in three interleaved rounds per side: faster on 69 of 69, median 2.69×, geometric mean 2.81×
with any single ratio capped at 10, minimum 1.22×. The minimum row in the report is `JSON.GET`
of the 10,000 short strings (68.94 µs against 84.28 µs). The whole-document read of the record
array is 1.26× (11,185 µs against 14,050 µs). `SAVE` of 500,010 keys takes 1.944 s against
2.464 s and loading takes 1.936 s against 3.085 s. Memory per shape is as in §5.2, except one
byte more for the 100-member object (451 bytes of `used_memory` growth against 450). Raw
results: `f-cand-*.json` and `report-final-graviton.md`.

**Real documents.** Eight JSON files from the external module's test data, each stored as one
key, on `Graviton host B` with the final code. Memory is `MEMORY USAGE` in bytes. Times are
server-side microseconds per call, module first and built-in second. Every document read back
equal to its source on both servers.

| File | JSON text, bytes | Module memory, bytes | Built-in memory, bytes | Ratio | `JSON.SET`, µs | `JSON.GET`, µs |
|---|---:|---:|---:|---:|---:|---:|
| `twitter.json` | 631,514 | 560,616 | 263,336 | 47% | 2,949 and 1,307 | 483 and 223 |
| `apache_builds.json` | 127,275 | 152,368 | 91,720 | 60% | 628 and 257 | 65.4 and 47.3 |
| `github_events.json` | 65,132 | 70,976 | 47,800 | 67% | 273 and 103 | 45.6 and 23.3 |
| `truenull.json` | 12,000 | 32,816 | 2,072 | 6% | 37.2 and 14.0 | 22.3 and 4.7 |
| `webxml.json` | 3,468 | 3,160 | 1,304 | 41% | 19.0 and 7.7 | 2.82 and 1.33 |
| `store.json` | 1,895 | 1,912 | 664 | 35% | 11.2 and 4.9 | 2.05 and 1.04 |
| `wikipedia.json` | 432 | 528 | 184 | 35% | 3.96 and 1.87 | 1.00 and 0.45 |
| `wikipedia_compact.json` | 309 | 528 | 184 | 35% | 3.73 and 1.80 | 1.00 and 0.45 |

The two files at 60% and 67% are dominated by long strings (URLs and descriptions), which
both implementations store as they are. `truenull.json` at 6% is the literal typed array of
§5.3. `JSON.SET` is 2.1 to 2.7 times as fast on every file, and `JSON.GET` 1.4 to 4.7 times.
Eight files from a test suite are not production data: they show the direction on real
shapes, not the ratio a given workload gets.

**Many clients.** `valkey-benchmark` with 50 connections, pipeline depth 16 and 8 client
threads over a unix socket, 100,000 profile documents, on `Graviton host B`. The server has two
cores and the client twenty. Figures are requests per second, the higher of two runs. No
command returned an error.

| Command | External module, requests/s | Built-in, requests/s | Ratio |
|---|---:|---:|---:|
| `JSON.SET` whole profile | 147,827 | 292,084 | 1.98× |
| `JSON.GET` whole profile | 332,594 | 630,517 | 1.90× |
| `JSON.GET $.address.city` | 399,255 | 856,164 | 2.14× |
| `JSON.NUMINCRBY $.age 1` | 399,308 | 748,503 | 1.87× |
| `JSON.SET $.name` | 399,361 | 665,484 | 1.67× |
| `JSON.ARRAPPEND $.tags` | 374,345 | 544,267 | 1.45× |

This run used one server on two cores with no I/O threads. Raw results for both runs:
`json-m0/bench/results/real-*.json`.

**x86-64 hardware, GitHub Actions.** The author's x86 hosts stayed unreachable, so the
workflow `.github/workflows/json-builtin.yml` on the fork's branch `json-builtin-ci` runs the
same benchmark on a GitHub-hosted runner. The runner has 4 vCPUs. Each server is pinned to two
of them and the client to a third, with three interleaved rounds per side. The external module
is built `-O3 -march=nehalem` at `60cfc5c`, the commit of the Graviton baseline. The runner's
processor changes between runs. Final code, run 43694898886, on an Intel Xeon 6975P-C:

| | External module | Built-in | Ratio |
|---|---:|---:|---:|
| Memory, all nine shapes | | | identical to Graviton (§5.2) |
| JSON operations faster than the module | | | 69 of 69, median 2.30×, geometric mean 2.46×, minimum 1.03× |
| `JSON.SET` whole profile record, µs | 4.41 | 1.89 | 2.33× |
| `JSON.GET $.address.city`, µs | 1.26 | 0.45 | 2.80× |
| `JSON.GET` of the 100,000-record array, µs | 10,910 | 10,552 | 1.03× |
| `SAVE`, 500,010 keys, seconds | 2.222 | 1.709 | 1.30× |
| Load (`DEBUG RELOAD NOSAVE`), seconds | 2.332 | 1.459 | 1.60× |
| `twitter.json` (632 KB): `JSON.SET` and `JSON.GET`, µs | 1,911 and 273 | 790 and 138 | 2.42× and 1.98× |

With 50 connections, as in the many-client run above, in requests per second:

| Command | External module | Built-in | Ratio |
|---|---:|---:|---:|
| `JSON.SET` whole profile | 199,455 | 385,802 | 1.93× |
| `JSON.GET` whole profile | 373,459 | 629,459 | 1.69× |
| `JSON.GET $.address.city` | 520,291 | 919,681 | 1.77× |
| `JSON.NUMINCRBY $.age 1` | 543,774 | 854,214 | 1.57× |
| `JSON.SET $.name` | 520,111 | 853,728 | 1.64× |
| `JSON.ARRAPPEND $.tags` | 497,843 | 663,717 | 1.33× |

Before the serialiser change of `2c17a3a01`, two runs on AMD EPYC 9V74 runners had the same
memory, a median of 2.29×, and the whole-document read of the record array at 0.97×, the only
operation slower than the module on any platform. A profile on the runner showed time in the
serialiser's member loop and in an out-of-line copy helper. The change made that read 10%
faster on Graviton and turned it into a gain on x86-64. The before and after runs were on
different processors, so the x86-64 comparison of the two is indicative. Raw results:
`json-m0/bench/results/gha-x86/` and `gha-x86-2/` (before) and `gha-x86-final/` (after).

The same workflow runs the correctness jobs of §11 on x86-64, on 32-bit x86, and on s390x
under qemu.

**History: x86-64 under emulation (superseded).** Before x86 hardware was available, x86-64
was checked on the Mac with the server compiled for x86-64 and run under Rosetta 2. This
exercised the x86-64 code the compiler generates. It was not a measurement of x86 hardware,
and the runner measurements above replace it. *Correctness:* the tree committed at
the time built for x86-64 with no warnings and passed the suites listed in §11. *Relative
cost:* both servers and the external module were built for x86-64 and measured in two
interleaved rounds at one fifth of the data size, with no core pinning on a busy laptop. The
built-in was faster on 69 of 69 operations, with a median of 2.71× and a minimum of 1.02× (`JSON.SET` of 100,000
integers). Memory under the macOS allocator was 45%, 46% and 38% of the module's on the three
record shapes, 34% on the record array, 17% and 27% on the two number arrays, 27% on integers
and 100% on short strings. These figures were indicative only. Raw results:
`json-m0/bench/results/x86r-*.json` and `report-x86-rosetta.md`.

**Not measured.**

- x86 hardware other than shared GitHub runners. No dedicated x86-64 host was
  reachable: `x86 host A`, `x86 host B` and `x86 host C` timed out on port 22 throughout.
- Speed on 32-bit x86 and on s390x. Both are tested for correctness only, and s390x runs under
  qemu.
- Production documents and real embeddings. Member-name sharing, string lengths and number
  formats in real workloads move the memory ratios.
- Scale: I/O threads, replication under load, cluster mode, and more than one server.
- Contended reference counts in the key table.
- The 3.8 KB document of the prototype's write measurements, and the engine-level cost of each
  write primitive as built.
- Fragmentation after a write-heavy workload.

**History: the prototype measurements (superseded).** Before implementation, a C11 prototype
of §5, §6 and the three write primitives (`json-m0/proto/`) and the real module were measured
on one Apple M3 Pro under macOS, single thread. The prototype's memory was the sum of
allocation sizes rounded to jemalloc size classes by arithmetic, compared with module bytes
under the macOS allocator. Its times were engine-level medians of 9 runs, compared with the
module's whole commands. A check of 7,547,454 comparisons under ASan and UBSan preceded each
run. The module API cost was measured with a three-command test module (`json-m0/apibench/`).
Full tables are in `json-m0/proto/RESULTS.md`. Those runs decided the layout, the key table,
the key cache and splice. Every figure from them in this document is labelled *prototype,
superseded*. The in-server figures above replace them, and they differ in three places: the
six-decimal embedding (58% then, 29% now), the short-string array (148% then, 100% now), and
reading back arrays of doubles (slower than the module then, 2.48× and 2.57× its speed now).
