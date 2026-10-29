#include "mybinlog/compression.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "bytes_internal.h"
#include "mybinlog/event_types.h"

#ifdef MYBINLOG_HAVE_ZSTD
#include <zstd.h>
#endif

namespace mybinlog {
namespace {

// QUERY post-header (13 bytes): database length at 8, status-variables length at 11.
constexpr size_t kQueryPostHeaderSize = 13;
constexpr size_t kQueryDbLenOffset = 8;
constexpr size_t kQueryStatusVarsLenOffset = 11;

// Rows post-header: 6-byte table id and 2-byte flags; v2 adds the extra-data block.
constexpr size_t kRowsPostHeaderV1Size = 8;

// MariaDB compression header byte (sql/log_event.cc, binlog_buf_compress).
constexpr uint8_t kCompressedFlag = 0x80;
constexpr uint8_t kAlgorithmMask = 0x70;
constexpr uint8_t kLengthOfLengthMask = 0x07;
constexpr size_t kMaxLengthOfLength = 4;

// TRANSACTION_PAYLOAD field types (libs/mysql/binlog/event/codecs/binary.h).
constexpr uint64_t kPayloadEndMark = 0;
constexpr uint64_t kPayloadSizeField = 1;
constexpr uint64_t kPayloadCompressionField = 2;
constexpr uint64_t kPayloadUncompressedSizeField = 3;

/** Bytes of the compressed body kept in the clear, before the compression header. */
Status ClearPrefixSize(EventType type, const uint8_t* body, size_t size, size_t* out) {
  ByteReader reader(body, size);
  if (type == EventType::kMariaDBQueryCompressedEvent) {
    if (size < kQueryPostHeaderSize) return Status::kTruncated;
    const size_t db_len = body[kQueryDbLenOffset];
    const size_t status_len = ReadU16Le(body + kQueryStatusVarsLenOffset);
    // Status variables, database name and its NUL; the statement text follows.
    const Status status = reader.Skip(kQueryPostHeaderSize + status_len + db_len + 1);
    if (status != Status::kOk) return status;
    *out = size - reader.remaining();
    return Status::kOk;
  }
  Status status = reader.Skip(kRowsPostHeaderV1Size);
  if (status != Status::kOk) return status;
  const bool v2 = type == EventType::kMariaDBWriteRowsCompressedEvent ||
                  type == EventType::kMariaDBUpdateRowsCompressedEvent ||
                  type == EventType::kMariaDBDeleteRowsCompressedEvent;
  if (v2) {
    uint16_t extra_len = 0;
    status = reader.ReadU16Le(&extra_len);
    if (status != Status::kOk) return status;
    // The length counts its own two bytes.
    if (extra_len < 2) return Status::kInvalidMetadata;
    status = reader.Skip(extra_len - 2U);
    if (status != Status::kOk) return status;
  }
  uint64_t columns = 0;
  status = reader.ReadPacked(&columns);
  if (status != Status::kOk) return status;
  if (columns > std::numeric_limits<size_t>::max()) return Status::kTruncated;
  const bool update = type == EventType::kMariaDBUpdateRowsCompressedEventV1 ||
                      type == EventType::kMariaDBUpdateRowsCompressedEvent;
  const size_t bitmap = BitmapBytes(static_cast<size_t>(columns));
  status = reader.Skip(bitmap);
  if (status != Status::kOk) return status;
  if (update) {
    status = reader.Skip(bitmap);
    if (status != Status::kOk) return status;
  }
  *out = size - reader.remaining();
  return Status::kOk;
}

/** The plain type a MariaDB compressed type expands to. */
EventType PlainType(EventType type) {
  switch (type) {
    case EventType::kMariaDBQueryCompressedEvent:
      return EventType::kQueryEvent;
    case EventType::kMariaDBWriteRowsCompressedEventV1:
      return EventType::kWriteRowsEventV1;
    case EventType::kMariaDBUpdateRowsCompressedEventV1:
      return EventType::kUpdateRowsEventV1;
    case EventType::kMariaDBDeleteRowsCompressedEventV1:
      return EventType::kDeleteRowsEventV1;
    case EventType::kMariaDBWriteRowsCompressedEvent:
      return EventType::kWriteRowsEvent;
    case EventType::kMariaDBUpdateRowsCompressedEvent:
      return EventType::kUpdateRowsEvent;
    default:
      return EventType::kDeleteRowsEvent;
  }
}

/** Inflate a zlib stream into exactly @p out_size bytes at @p dst, consuming all input. */
Status InflateExact(const uint8_t* src, size_t src_size, uint8_t* dst, size_t out_size) {
  constexpr size_t kMaxChunk = std::numeric_limits<uInt>::max();
  if (src_size > kMaxChunk || out_size > kMaxChunk) return Status::kLimitExceeded;
  z_stream stream{};
  stream.next_in = const_cast<Bytef*>(src);
  stream.avail_in = static_cast<uInt>(src_size);
  stream.next_out = dst;
  stream.avail_out = static_cast<uInt>(out_size);
  if (inflateInit(&stream) != Z_OK) return Status::kLimitExceeded;
  const int result = inflate(&stream, Z_FINISH);
  const bool exact = result == Z_STREAM_END && stream.avail_out == 0 && stream.avail_in == 0;
  inflateEnd(&stream);
  return exact ? Status::kOk : Status::kMalformedValue;
}

#ifdef MYBINLOG_HAVE_ZSTD
struct DctxDeleter {
  void operator()(ZSTD_DCtx* ctx) const { ZSTD_freeDCtx(ctx); }
};

/** Expand every zstd frame of the input; output past @p max_size is kLimitExceeded. */
Status ZstdExpand(const uint8_t* src, size_t src_size, size_t max_size, size_t size_hint,
                  std::vector<uint8_t>* out) {
  const std::unique_ptr<ZSTD_DCtx, DctxDeleter> ctx(ZSTD_createDCtx());
  if (!ctx) return Status::kLimitExceeded;
  // One byte past the ceiling proves the stream overruns it.
  const size_t cap = max_size == std::numeric_limits<size_t>::max() ? max_size : max_size + 1;
  std::vector<uint8_t> buffer;
  size_t written = 0;
  size_t pending = 1;  // ZSTD_decompressStream's hint; 0 once a frame is complete.
  ZSTD_inBuffer in{src, src_size, 0};
  while (true) {
    if (written == buffer.size()) {
      if (buffer.size() >= cap) break;
      const size_t want =
          buffer.empty() ? std::max<size_t>(size_hint, ZSTD_DStreamOutSize()) : buffer.size() * 2;
      buffer.resize(std::min(want, cap));
    }
    ZSTD_outBuffer output{buffer.data(), buffer.size(), written};
    pending = ZSTD_decompressStream(ctx.get(), &output, &in);
    if (ZSTD_isError(pending)) return Status::kMalformedValue;
    written = output.pos;
    // A frame ends flushed with the input; another call would expect a new frame.
    if (in.pos == in.size && pending == 0) break;
    // Input exhausted mid-frame: the decoder had room and still wants more.
    if (in.pos == in.size && output.pos < output.size) break;
  }
  if (written > max_size) return Status::kLimitExceeded;
  if (pending != 0) return Status::kMalformedValue;
  buffer.resize(written);
  *out = std::move(buffer);
  return Status::kOk;
}
#endif

}  // namespace

bool HasZstdSupport() noexcept {
#ifdef MYBINLOG_HAVE_ZSTD
  return true;
#else
  return false;
#endif
}

bool IsMariaDbCompressedEvent(uint8_t type_code) noexcept {
  return type_code >= static_cast<uint8_t>(EventType::kMariaDBQueryCompressedEvent) &&
         type_code <= static_cast<uint8_t>(EventType::kMariaDBDeleteRowsCompressedEvent);
}

Status DecompressMariaDbEvent(const EventHeader& header, const uint8_t* body, size_t size,
                              size_t max_size, DecompressedEvent* out) {
  if (!IsMariaDbCompressedEvent(header.type_code)) return Status::kUnsupported;
  if (body == nullptr) return Status::kTruncated;
  const auto type = static_cast<EventType>(header.type_code);
  size_t prefix = 0;
  Status status = ClearPrefixSize(type, body, size, &prefix);
  if (status != Status::kOk) return status;

  ByteReader reader(body + prefix, size - prefix);
  uint8_t flags = 0;
  status = reader.ReadU8(&flags);
  if (status != Status::kOk) return status;
  if ((flags & kCompressedFlag) == 0) return Status::kMalformedValue;
  if ((flags & kAlgorithmMask) != 0) return Status::kUnsupported;
  const size_t length_bytes = flags & kLengthOfLengthMask;
  if (length_bytes == 0 || length_bytes > kMaxLengthOfLength) return Status::kMalformedValue;
  const uint8_t* length_field = nullptr;
  status = reader.ReadBytes(length_bytes, &length_field);
  if (status != Status::kOk) return status;
  size_t original = 0;
  for (size_t i = 0; i < length_bytes; ++i) original = (original << 8) | length_field[i];
  if (original == 0) return Status::kMalformedValue;

  if (original > max_size || prefix > max_size - original) return Status::kLimitExceeded;
  const uint64_t trailer = header.event_length >= kEventHeaderSize + uint64_t{size}
                               ? header.event_length - kEventHeaderSize - uint64_t{size}
                               : 0;
  const uint64_t event_length = kEventHeaderSize + uint64_t{prefix} + original + trailer;
  if (event_length > std::numeric_limits<uint32_t>::max()) return Status::kLimitExceeded;

  std::vector<uint8_t> plain(prefix + original);
  std::copy(body, body + prefix, plain.begin());
  status = InflateExact(reader.position(), reader.remaining(), plain.data() + prefix, original);
  if (status != Status::kOk) return status;

  out->header = header;
  out->header.type_code = static_cast<uint8_t>(PlainType(type));
  out->header.event_length = static_cast<uint32_t>(event_length);
  out->body = std::move(plain);
  return Status::kOk;
}

Status ParseTransactionPayloadEvent(const EventHeader& /*header*/, const uint8_t* body, size_t size,
                                    TransactionPayloadEvent* out) {
  if (body == nullptr) return Status::kTruncated;
  ByteReader reader(body, size);
  TransactionPayloadEvent event;
  bool have_size = false;
  bool have_compression = false;
  while (true) {
    uint64_t type = 0;
    Status status = reader.ReadPacked(&type);
    if (status != Status::kOk) return status;
    if (type == kPayloadEndMark) break;
    uint64_t length = 0;
    status = reader.ReadPacked(&length);
    if (status != Status::kOk) return status;
    // Known fields hold one packed integer; the source ignores their length too.
    uint64_t value = 0;
    switch (type) {
      case kPayloadSizeField:
        status = reader.ReadPacked(&event.payload_size);
        have_size = true;
        break;
      case kPayloadCompressionField:
        status = reader.ReadPacked(&value);
        if (status != Status::kOk) return status;
        if (value != static_cast<uint8_t>(PayloadCompression::kZstd) &&
            value != static_cast<uint8_t>(PayloadCompression::kNone)) {
          return Status::kUnsupported;
        }
        event.compression = static_cast<PayloadCompression>(value);
        have_compression = true;
        break;
      case kPayloadUncompressedSizeField:
        status = reader.ReadPacked(&event.uncompressed_size);
        event.has_uncompressed_size = true;
        break;
      default:
        if (length > reader.remaining()) return Status::kTruncated;
        status = reader.Skip(static_cast<size_t>(length));
        break;
    }
    if (status != Status::kOk) return status;
  }
  if (!have_size || !have_compression) return Status::kInvalidMetadata;
  if (event.payload_size > reader.remaining()) return Status::kTruncated;
  event.payload = reader.position();
  *out = event;
  return Status::kOk;
}

Status DecompressTransactionPayload(const TransactionPayloadEvent& event, size_t max_size,
                                    std::vector<uint8_t>* events) {
  if (event.has_uncompressed_size && event.uncompressed_size > max_size) {
    return Status::kLimitExceeded;
  }
  if (event.payload == nullptr && event.payload_size != 0) return Status::kTruncated;
  // payload_size never exceeds the parsed body, so it fits in size_t.
  const auto payload_size = static_cast<size_t>(event.payload_size);
  std::vector<uint8_t> expanded;
  switch (event.compression) {
    case PayloadCompression::kNone:
      if (payload_size > max_size) return Status::kLimitExceeded;
      expanded.assign(event.payload, event.payload + payload_size);
      break;
    case PayloadCompression::kZstd: {
#ifdef MYBINLOG_HAVE_ZSTD
      const size_t hint =
          event.has_uncompressed_size ? static_cast<size_t>(event.uncompressed_size) : payload_size;
      const Status status = ZstdExpand(event.payload, payload_size, max_size, hint, &expanded);
      if (status != Status::kOk) return status;
      break;
#else
      return Status::kUnsupported;
#endif
    }
    default:
      return Status::kUnsupported;
  }
  if (event.has_uncompressed_size && expanded.size() != event.uncompressed_size) {
    return Status::kMalformedValue;
  }
  *events = std::move(expanded);
  return Status::kOk;
}

Status NextPayloadEvent(const uint8_t* data, size_t size, size_t* offset, PayloadEventView* out) {
  if (data == nullptr || *offset > size) return Status::kTruncated;
  const size_t remaining = size - *offset;
  EventHeader header;
  const Status status = ParseEventHeader(data + *offset, remaining, &header);
  if (status != Status::kOk) return status;
  if (header.event_length < kEventHeaderSize) return Status::kMalformedValue;
  if (header.event_length > remaining) return Status::kTruncated;
  out->header = header;
  out->body = data + *offset + kEventHeaderSize;
  out->size = header.event_length - kEventHeaderSize;
  *offset += header.event_length;
  return Status::kOk;
}

}  // namespace mybinlog
