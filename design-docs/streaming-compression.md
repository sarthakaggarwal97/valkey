# Valkey Compressed Stream

Tracking issue: [#3195](https://github.com/valkey-io/valkey/issues/3195).
Initial implementation: [#3531](https://github.com/valkey-io/valkey/pull/3531).

Valkey Compressed Stream (VCS) is a small, versioned envelope around a
codec-native compressed frame. It lets consumers identify the codec and the
semantics of the decompressed bytes before parsing the inner stream.

For RDB persistence, VCS is an outer physical format. The bytes produced by the
RDB serializer remain unchanged and are compressed without making the codec or
stream layer aware of RDB objects.

```text
RDB serializer -> compress rio -> VCS writer -> codec frame -> dump.rdb
RDB parser     <- decompress rio <- VCS reader <- codec frame <- dump.rdb
```

## Version 1 Envelope

The v1 envelope is seven bytes followed immediately by one codec frame.

| Offset | Size | Field | Version 1 value |
| ---: | ---: | --- | --- |
| 0 | 3 | Magic | ASCII `VCS` (`56 43 53` in hexadecimal) |
| 3 | 1 | Envelope version | `0x01` |
| 4 | 1 | Codec ID | Registered value from the codec table |
| 5 | 1 | Reserved | `0x00` |
| 6 | 1 | Stream-kind ID | Registered value from the stream-kind table |
| 7 | variable | Payload | One frame in the selected codec's standard format |

All v1 fields are bytes. A later version that adds multi-byte fields must use
network byte order.

### Codec IDs

| ID | Name | Payload |
| ---: | --- | --- |
| `0x00` | Invalid | Reserved; writers and readers must reject it |
| `0x01` | LZ4 | Standard LZ4 frame |
| `0x02`-`0xff` | Unassigned | Writers and readers must reject them |

Codec IDs are wire identifiers. They are deliberately independent of
`compressionAlgo` values used inside the server.

### Stream-Kind IDs

The stream kind identifies the semantics of the bytes after decompression. It
prevents a valid compressed stream from being passed to the wrong parser.

| ID | Name | Decompressed bytes |
| ---: | --- | --- |
| `0x00` | Invalid | Reserved; writers and readers must reject it |
| `0x01` | RDB | One complete logical RDB stream |
| `0x02`-`0xff` | Unassigned | Writers and readers must reject them |

New IDs must be added to this registry and to both the writer and reader
validation before use. Assigned IDs must never be reused for different
semantics.

## Reader Rules

A reader first classifies the input from the magic bytes:

- If all three magic bytes match `VCS`, the input is VCS. A truncated envelope,
  unknown version, unknown codec, nonzero reserved byte, or unknown/mismatched
  stream kind is an incompatible stream and must not fall back to plaintext.
- If the magic does not match, a caller configured for passthrough may replay
  the probe bytes and continue as an uncompressed stream.
- A finite consumer must decode one complete codec frame and reject unread
  decoded bytes, a truncated frame, and bytes after the frame.
- Codec checksum verification may be disabled only by an explicit caller
  option. Framing, decompression, stream-kind validation, and exact-end
  validation remain active.

Unknown identifiers fail closed so adding a codec or stream kind cannot make an
older binary silently interpret new data under old semantics.

## RDB Profile

`rdbcompression lz4-stream` writes a VCS v1 stream with codec `0x01` and stream
kind `0x01`. The decompressed payload is a normal RDB, including its
`VALKEY`/`REDIS0` header, EOF opcode, and eight-byte checksum trailer.

For this physical format:

- The logical RDB CRC64 trailer is zero.
- `rdbchecksum yes` enables both LZ4 block and content checksums.
- `rdbchecksum no` omits both LZ4 checksums.
- Loaders discover checksum presence from the LZ4 frame header, not from the
  VCS envelope.
- Valkey's writer uses linked 64 KiB LZ4 blocks and the codec's default
  compression level. Readers accept valid standard LZ4 frames independently of
  those writer tuning choices.
- Per-string LZF is skipped while whole-stream compression is active.

Plain RDB files remain the default and continue to start with `VALKEY` or
`REDIS0`. VCS RDB files start with `VCS`, so binaries without VCS support reject
them before parsing the logical RDB. A VCS RDB received from a compatible
full-sync source also cannot be reused directly as an AOF RDB base; Valkey
loads it and creates a plain base with `BGREWRITEAOF`.

## Extension Rules

Changes to the envelope layout or existing field semantics require a new
envelope version. Adding a codec or stream kind does not require a version bump,
but it does require:

1. A unique ID in the relevant registry above.
2. Writer and reader support for that ID.
3. Tests that unknown IDs still fail closed and that the new ID cannot be
   confused with an existing consumer.
