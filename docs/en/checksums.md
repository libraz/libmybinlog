# Checksums

A binlog event may end with a 4-byte CRC32 trailer. Whether it does is a property of the file, not of each event, and the file declares it in its FORMAT_DESCRIPTION event (FDE).

## Where the algorithm comes from

The FDE that opens each binlog file carries the algorithm as one byte just before its own 4-byte trailer (`mybinlog/checksum.h`):

| `ChecksumAlgorithm` | Wire byte | Events after the FDE |
|---|---|---|
| `kOff` | 0 | No trailer. |
| `kCrc32` | 1 | Every event ends with a CRC32 of the bytes before it. |
| `kUndefined` | 255 (not written) | A server that predates checksums: no algorithm byte and no trailer on any event, the FDE included. |

The FDE of a checksum-aware server always carries its own trailer, even when it declares OFF. Servers older than MySQL 5.6.1 and MariaDB 5.3.0 write neither the byte nor the trailer; `ServerVersionPredatesChecksums()` tells them apart by the FDE's server version.

The algorithm changes only at an FDE and applies until the next one, so a stream that crosses files re-derives it at each FDE. It is never taken from the `binlog_checksum` server variable, which can change while older files written under the previous value are still being read.

## What the decoder does

The decoder frames each event under the algorithm in effect and, under CRC32, compares the trailer:

- With `DecoderOptions::verify_checksum = true` (the default), a mismatch fails with `kChecksumMismatch`. So does a checksummed frame too short to hold the trailer.
- With `verify_checksum = false`, events are still framed by the algorithm, so the trailer is still excluded from the body, but the CRC is not compared. Use this when another layer has verified the bytes.
- `FormatDescriptionEvent::checksum_algorithm` reports what each FDE declared.
- Event bodies handed to the caller (`UnknownEvent::body`, `TableRowsEvent::body`) never include the trailer.

A replication stream opens with an artificial ROTATE that comes before any FDE. It is framed by `DecoderOptions::default_checksum`, which should be the algorithm the replica announced to the source. Under OFF framing that ROTATE counts as checksummed only when its CRC verifies.

Events inside a TRANSACTION_PAYLOAD never have a trailer, whatever the file's algorithm.

## ChecksumTracker

Code that frames events itself uses `ChecksumTracker`, the same component the decoder runs. Feed it every event of one stream in order:

```cpp
mybinlog::ChecksumTracker tracker;  // assumes kCrc32 before the first FDE
mybinlog::FrameLayout layout;
mybinlog::Status status = tracker.OnEvent(header, frame, header.event_length, &layout);
if (status == mybinlog::Status::kOk) {
  const uint8_t* body = frame + mybinlog::kEventHeaderSize;
  size_t body_size = layout.body_length();  // trailer excluded
}
```

`OnEvent()` re-derives the algorithm when the event is an FDE, then frames it. `FrameLayout::payload_length` is the header plus body and `has_trailer` says whether a trailer follows. `SetDefaultAlgorithm()` sets the algorithm assumed before the first FDE (and after `Reset()`), `SetVerify(false)` skips the comparison, and `algorithm()` returns the algorithm in effect for the next non-FDE event.

`ReadFdeChecksumAlgorithm()` reads the algorithm from a complete FDE frame without a tracker; it works on the whole frame because the trailer's presence is what it decides.

A tracker serves one stream and is used from one thread at a time.

## CRC32 helpers

`mybinlog/crc32.h` exposes the CRC32 the trailers use (the zlib polynomial): `ComputeCrc32(data, size)` for a range and `UpdateCrc32(crc, data, size)` to extend a running value, starting from 0.
