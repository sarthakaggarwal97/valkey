# Built-in JSON module

This directory holds the JSON data type and the `JSON.*` commands. It is a
module in the technical sense: it uses only the public module API
(`valkeymodule.h`) and registers itself under the name `json`. Unlike other
modules it is compiled into `valkey-server` and loaded at startup, so no
`loadmodule` line is needed.

It replaces the external valkey-json module and is compatible with it:

- The data type is `ReJSON-RL`, encoding version 3. RDB files, `DUMP`
  payloads and the replication stream are read and written in the same form.
- The same 24 commands, command flags, key specifications, ACL category
  (`json`), keyspace events, `INFO` section and configuration names.
- The shared API `JSON_GetValue`, which other modules use to read documents.

## Building and turning it off

The module is built by default. `make BUILD_JSON=no` (or `-DBUILD_JSON=OFF`
with CMake) leaves it out of the binary.

The engine runs on 64-bit and 32-bit targets of either byte order. A
document is one 8-byte word. Where a pointer is 8 bytes the word is stored in
the key's value pointer. On a 32-bit target it is stored in a heap cell of 8
bytes that the value pointer refers to.

At run time, `json-enabled no` in the configuration starts the server without
it, for example to load an external JSON module instead. Loading both is an
error: two modules cannot share a name.

## Configuration

| Name | Default | Meaning |
|---|---|---|
| `json.max-document-size` | 0 (no limit) | Largest document, in bytes of memory |
| `json.max-path-limit` | 128 | Deepest nesting a document may have |
| `json.max-packed-bytes` | 4096 | A container up to this size is stored as one packed block |
| `json.max-packed-entries` | 128 | A container up to this many entries is stored as one packed block |

## Files

| File | Contents |
|---|---|
| `json_module.c`, `json_module.h` | Entry point, data type callbacks (RDB, AOF, copy, digest, defrag, free), configuration, statistics, shared API. The only file that connects the engine to the server |
| `json_cmd.c` | The commands: argument parsing, replies, replication and events |
| `json_path.c`, `json_path.h` | Path compiler and evaluator, legacy and JSONPath syntax |
| `json_engine.c`, `json_engine.h`, `json_engine_format.h` | In-memory document format, parser, serialiser, key table, and every change to a document |
| `json_num.c`, `json_num.h` | Shortest round-trip number printing and correctly rounded number parsing |

`json_engine.c`, `json_path.c` and `json_num.c` include no server header and
take their allocator from the caller, so they also build as standalone test
programs.

## How a document is stored

A small container (an object or array up to the two `max-packed` limits) is
one contiguous block: a tag byte per scalar, short headers per nested
container, member names replaced by integer IDs from a table shared by all
documents. A larger container is a node of 8-byte words, each holding a small
value directly or pointing to a child. Arrays whose elements are all integers,
decimals of one scale, doubles or short strings are stored as plain C arrays
of the narrowest element width.

A number keeps its exact source text. Integers and decimals of up to 18
significant digits are held in binary and printed back digit for digit.
Anything else is kept as text.

## Tests

- `tests/unit/type/json*.tcl`, `tests/integration/json-*.tcl`: the test suite
  that runs with `./runtest`.
- `tests/engine`, `tests/path`, `tests/num`, `tests/cmd` in this directory:
  standalone programs and scripts used during development. They compare the
  engine with an independent model in Python, and the path evaluator and
  commands with a server running the external module. They are not part of
  `make test`.
