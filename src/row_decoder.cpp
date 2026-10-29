#include "mybinlog/row_decoder.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "bytes_internal.h"
#include "decimal_internal.h"
#include "temporal_internal.h"

namespace mybinlog {

namespace {

// Collation id of `binary`: a character column in it holds bytes, not text.
constexpr uint32_t kBinaryCollation = 63;
constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();
constexpr uint32_t kMaxLengthPrefixWidth = 4;
constexpr size_t kMaxBitBytes = 8;
// Field_typed_array stores its value like JSON with a fixed 4-byte length.
constexpr uint32_t kTypedArrayPrefixWidth = 4;
// Json_diff_vector::write_binary opens with a 4-byte length (sql-common/json_diff.cc).
constexpr uint32_t kJsonDiffPrefixWidth = 4;

// binlog_row_value_options bit PARTIAL_JSON_UPDATES; upstream rejects any other bit.
constexpr uint64_t kPartialJsonUpdates = 1;

// v2 extra row info type codes (libs/mysql/binlog/event/rows_event.h).
constexpr uint8_t kExtraRowInfoNdb = 0;
constexpr uint8_t kExtraRowInfoPartition = 1;
// An NDB entry's length byte counts itself and the format byte.
constexpr uint8_t kExtraRowInfoNdbHeader = 2;

// MariaDB COMPRESSED value header (sql/field_comp.cc): method in the high
// nibble, bit 3 selects a raw deflate stream, bits 0-2 the original-length width.
constexpr unsigned kStoredMethod = 0;
constexpr unsigned kZlibMethod = 8;
constexpr uint8_t kRawDeflateBit = 0x08;
constexpr uint8_t kLengthWidthMask = 0x07;

// Ceilings that apply to the value being decoded.
struct ValueLimits {
  size_t value;     // Any materialized payload.
  size_t inflated;  // A decompressed payload (never above value).
};

ValueLimits LimitsFor(const DecodeOptions& options) {
  size_t value = options.max_value_bytes == 0 ? kUnlimited : options.max_value_bytes;
  if (options.budget != nullptr) value = std::min(value, options.budget->Remaining());
  const size_t inflated =
      options.max_decompressed_bytes == 0 ? value : std::min(value, options.max_decompressed_bytes);
  return {value, inflated};
}

bool HoldsBytes(const TableMapColumn& column) {
  return !column.has_charset || column.charset == kBinaryCollation;
}

// Length prefix of @p width (1..4) little-endian bytes, then that many bytes.
Status ReadPrefixed(ByteReader* reader, uint32_t width, const uint8_t** data, size_t* length) {
  if (width < 1 || width > kMaxLengthPrefixWidth) return Status::kInvalidMetadata;
  const uint8_t* prefix = nullptr;
  Status status = reader->ReadBytes(width, &prefix);
  if (status != Status::kOk) return status;
  uint64_t value = 0;
  for (uint32_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(prefix[i]) << (8 * i);
  if (value > reader->remaining()) return Status::kTruncated;
  *length = static_cast<size_t>(value);
  return reader->ReadBytes(*length, data);
}

Status SetPayload(const uint8_t* data, size_t length, Value::Kind kind, const ValueLimits& limits,
                  Value* out) {
  if (length > limits.value) return Status::kLimitExceeded;
  out->kind = kind;
  out->bytes.assign(reinterpret_cast<const char*>(data), length);
  return Status::kOk;
}

Status Inflate(const uint8_t* data, size_t length, bool raw, size_t original, std::string* out) {
  if (length > std::numeric_limits<uInt>::max() || original > std::numeric_limits<uInt>::max()) {
    return Status::kLimitExceeded;
  }
  std::string inflated(original, '\0');
  z_stream stream{};
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(length);
  stream.next_out = reinterpret_cast<Bytef*>(&inflated[0]);
  stream.avail_out = static_cast<uInt>(original);
  if (inflateInit2(&stream, raw ? -MAX_WBITS : MAX_WBITS) != Z_OK) return Status::kLimitExceeded;
  const int result = inflate(&stream, Z_FINISH);
  const bool complete = result == Z_STREAM_END && stream.total_out == original;
  const bool trailing = stream.avail_in != 0;
  inflateEnd(&stream);
  if (!complete || trailing) return Status::kMalformedValue;
  *out = std::move(inflated);
  return Status::kOk;
}

// One MariaDB COMPRESSED payload (the bytes after the column's own length
// prefix). @p field_limit is the column's declared maximum length.
Status DecodeCompressed(const uint8_t* data, size_t length, size_t field_limit,
                        const ValueLimits& limits, std::string* out) {
  if (length == 0) {  // An empty value is stored without a header.
    out->clear();
    return Status::kOk;
  }
  const uint8_t header = data[0];
  const unsigned method = header >> 4;
  if (method == kStoredMethod) {
    if ((header & 0x0F) != 0) return Status::kMalformedValue;
    if (length - 1 > limits.value) return Status::kLimitExceeded;
    out->assign(reinterpret_cast<const char*>(data + 1), length - 1);
    return Status::kOk;
  }
  if (method != kZlibMethod) return Status::kUnsupported;

  const size_t width = header & kLengthWidthMask;
  if (width == 0 || width > kMaxLengthPrefixWidth || length - 1 <= width) {
    return Status::kMalformedValue;
  }
  uint64_t original = 0;
  for (size_t i = 0; i < width; ++i) original = (original << 8) | data[1 + i];
  if (original == 0 || original > field_limit) return Status::kMalformedValue;
  if (original > limits.inflated) return Status::kLimitExceeded;
  return Inflate(data + 1 + width, length - 1 - width, (header & kRawDeflateBit) != 0,
                 static_cast<size_t>(original), out);
}

// The prefix width of a CHAR / VAR_STRING value: upstream folds length bits
// 8-9 of a CHAR longer than 255 into the real-type byte (XORed).
uint32_t StringPrefixWidth(uint32_t meta) {
  const uint32_t max_length = (((meta >> 4) & 0x300) ^ 0x300) + (meta & 0xFF);
  return max_length > 255 ? 2 : 1;
}

Status DecodeInteger(const TableMapColumn& column, ByteReader* reader, size_t width, Value* out) {
  const uint8_t* data = nullptr;
  const Status status = reader->ReadBytes(width, &data);
  if (status != Status::kOk || out == nullptr) return status;
  uint64_t raw = 0;
  for (size_t i = 0; i < width; ++i) raw |= static_cast<uint64_t>(data[i]) << (8 * i);
  if (column.is_unsigned) {
    out->kind = Value::Kind::kUInt;
    out->u = raw;
    return Status::kOk;
  }
  const uint64_t sign = uint64_t{1} << (8 * width - 1);
  out->kind = Value::Kind::kInt;
  out->i = static_cast<int64_t>((raw ^ sign) - sign);
  return Status::kOk;
}

Status DecodeFloating(ByteReader* reader, bool is_double, Value* out) {
  if (is_double) {
    uint64_t bits = 0;
    const Status status = reader->ReadU64Le(&bits);
    if (status != Status::kOk || out == nullptr) return status;
    double value = 0;
    static_assert(sizeof(value) == sizeof(bits), "IEEE double");
    std::memcpy(&value, &bits, sizeof(value));
    out->kind = Value::Kind::kDouble;
    out->d = value;
    return Status::kOk;
  }
  uint32_t bits = 0;
  const Status status = reader->ReadU32Le(&bits);
  if (status != Status::kOk || out == nullptr) return status;
  float value = 0;
  static_assert(sizeof(value) == sizeof(bits), "IEEE float");
  std::memcpy(&value, &bits, sizeof(value));
  out->kind = Value::Kind::kDouble;
  out->d = static_cast<double>(value);
  return Status::kOk;
}

// ENUM (1..2 bytes) or SET (1..8 bytes), little-endian.
Status DecodeEnumOrSet(ColumnType real, uint32_t meta, ByteReader* reader, Value* out) {
  const uint32_t width = meta & 0xFF;
  const uint32_t max_width = real == ColumnType::kEnum ? 2 : 8;
  if (width < 1 || width > max_width) return Status::kInvalidMetadata;
  const uint8_t* data = nullptr;
  const Status status = reader->ReadBytes(width, &data);
  if (status != Status::kOk || out == nullptr) return status;
  uint64_t value = 0;
  for (uint32_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(data[i]) << (8 * i);
  out->kind = real == ColumnType::kEnum ? Value::Kind::kEnum : Value::Kind::kSet;
  out->u = value;
  return Status::kOk;
}

// BIT(M), metadata `bytes << 8 | bits`: (M + 7) / 8 big-endian bytes.
Status DecodeBit(uint32_t meta, ByteReader* reader, Value* out) {
  const size_t bits = static_cast<size_t>((meta >> 8) & 0xFF) * 8 + (meta & 0xFF);
  const size_t width = (bits + 7) / 8;
  if (bits == 0 || width > kMaxBitBytes) return Status::kInvalidMetadata;
  const uint8_t* data = nullptr;
  const Status status = reader->ReadBytes(width, &data);
  if (status != Status::kOk || out == nullptr) return status;
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value = (value << 8) | data[i];
  out->kind = Value::Kind::kBit;
  out->u = value;
  return Status::kOk;
}

Status DecodeNewDecimal(uint32_t meta, ByteReader* reader, const ValueLimits& limits, Value* out) {
  size_t width = 0;
  Status status = internal::DecimalWidth(meta, &width);
  if (status != Status::kOk) return status;
  const uint8_t* data = nullptr;
  status = reader->ReadBytes(width, &data);
  if (status != Status::kOk || out == nullptr) return status;
  status = internal::DecodeDecimal(meta, data, &out->bytes);
  if (status != Status::kOk) return status;
  if (out->bytes.size() > limits.value) return Status::kLimitExceeded;
  out->kind = Value::Kind::kDecimal;
  return Status::kOk;
}

Status DecodeTemporalColumn(ColumnType type, uint32_t meta, ByteReader* reader, Value* out) {
  size_t width = 0;
  Status status = internal::TemporalWidth(type, meta, &width);
  if (status != Status::kOk) return status;
  const uint8_t* data = nullptr;
  status = reader->ReadBytes(width, &data);
  if (status != Status::kOk || out == nullptr) return status;
  return internal::DecodeTemporal(type, meta, data, out);
}

// A length-prefixed value whose payload is character data or bytes.
Status DecodeCharacter(const TableMapColumn& column, uint32_t prefix_width, ByteReader* reader,
                       const ValueLimits& limits, Value* out) {
  const uint8_t* data = nullptr;
  size_t length = 0;
  const Status status = ReadPrefixed(reader, prefix_width, &data, &length);
  if (status != Status::kOk || out == nullptr) return status;
  const Value::Kind kind = HoldsBytes(column) ? Value::Kind::kBytes : Value::Kind::kText;
  return SetPayload(data, length, kind, limits, out);
}

// A length-prefixed value returned as @p kind without interpretation (JSON,
// GEOMETRY, VECTOR, TYPED_ARRAY).
Status DecodeOpaque(uint32_t prefix_width, ByteReader* reader, const ValueLimits& limits,
                    Value::Kind kind, Value* out) {
  const uint8_t* data = nullptr;
  size_t length = 0;
  const Status status = ReadPrefixed(reader, prefix_width, &data, &length);
  if (status != Status::kOk || out == nullptr) return status;
  return SetPayload(data, length, kind, limits, out);
}

// A PARTIAL_UPDATE_ROWS JSON diff vector, kept with its length prefix.
Status DecodeJsonDiff(ByteReader* reader, const ValueLimits& limits, Value* out) {
  uint32_t length = 0;
  Status status = reader->ReadU32Le(&length);
  if (status != Status::kOk) return status;
  const uint8_t* data = nullptr;
  status = reader->ReadBytes(length, &data);
  if (status != Status::kOk || out == nullptr) return status;
  // The payload follows its prefix contiguously.
  return SetPayload(data - kJsonDiffPrefixWidth, kJsonDiffPrefixWidth + size_t{length},
                    Value::Kind::kJsonDiff, limits, out);
}

Status DecodeCompressedColumn(const TableMapColumn& column, uint32_t prefix_width,
                              size_t field_limit, ByteReader* reader, const ValueLimits& limits,
                              Value* out) {
  const uint8_t* data = nullptr;
  size_t length = 0;
  Status status = ReadPrefixed(reader, prefix_width, &data, &length);
  if (status != Status::kOk || out == nullptr) return status;
  status = DecodeCompressed(data, length, field_limit, limits, &out->bytes);
  if (status != Status::kOk) return status;
  out->kind = HoldsBytes(column) ? Value::Kind::kBytes : Value::Kind::kText;
  return Status::kOk;
}

// Consumes one non-NULL value of @p column. With @p out null the value is
// only measured: metadata is validated, the payload is not interpreted.
Status DecodeValue(const TableMapColumn& column, ByteReader* reader, const ValueLimits& limits,
                   Value* out) {
  const uint32_t meta = column.meta;
  switch (column.type) {
    case ColumnType::kTiny:
      return DecodeInteger(column, reader, 1, out);
    case ColumnType::kShort:
      return DecodeInteger(column, reader, 2, out);
    case ColumnType::kInt24:
      return DecodeInteger(column, reader, 3, out);
    case ColumnType::kLong:
      return DecodeInteger(column, reader, 4, out);
    case ColumnType::kLongLong:
      return DecodeInteger(column, reader, 8, out);
    case ColumnType::kFloat:
      return DecodeFloating(reader, false, out);
    case ColumnType::kDouble:
      return DecodeFloating(reader, true, out);
    case ColumnType::kNewDecimal:
      return DecodeNewDecimal(meta, reader, limits, out);
    case ColumnType::kBit:
      return DecodeBit(meta, reader, out);

    case ColumnType::kYear:
    case ColumnType::kDate:
    case ColumnType::kNewDate:
    case ColumnType::kTime:
    case ColumnType::kDatetime:
    case ColumnType::kTimestamp:
    case ColumnType::kTimestamp2:
    case ColumnType::kDatetime2:
    case ColumnType::kTime2:
      return DecodeTemporalColumn(column.type, meta, reader, out);

    case ColumnType::kEnum:
    case ColumnType::kSet:
      return DecodeEnumOrSet(column.type, meta, reader, out);
    case ColumnType::kString: {
      const ColumnType real = RealType(column);
      if (real != ColumnType::kString) return DecodeEnumOrSet(real, meta, reader, out);
      return DecodeCharacter(column, StringPrefixWidth(meta), reader, limits, out);
    }
    case ColumnType::kVarString:
      return DecodeCharacter(column, StringPrefixWidth(meta), reader, limits, out);
    case ColumnType::kVarchar:
      return DecodeCharacter(column, meta > 255 ? 2 : 1, reader, limits, out);
    case ColumnType::kTinyBlob:
    case ColumnType::kMediumBlob:
    case ColumnType::kLongBlob:
    case ColumnType::kBlob:
      return DecodeCharacter(column, meta, reader, limits, out);

    case ColumnType::kJson:
      return DecodeOpaque(meta, reader, limits, Value::Kind::kJson, out);
    case ColumnType::kGeometry:
    case ColumnType::kVector:
      return DecodeOpaque(meta, reader, limits, Value::Kind::kBytes, out);
    case ColumnType::kTypedArray:
      return DecodeOpaque(kTypedArrayPrefixWidth, reader, limits, Value::Kind::kBytes, out);

    case ColumnType::kVarcharCompressed:
      return DecodeCompressedColumn(column, meta > 255 ? 2 : 1, meta, reader, limits, out);
    case ColumnType::kBlobCompressed: {
      if (meta < 1 || meta > kMaxLengthPrefixWidth) return Status::kInvalidMetadata;
      const size_t field_limit = static_cast<size_t>((uint64_t{1} << (8 * meta)) - 1);
      return DecodeCompressedColumn(column, meta, field_limit, reader, limits, out);
    }

    case ColumnType::kDecimal:  // Pre-5.0 DECIMAL; no current server writes it.
      return Status::kUnsupported;
  }
  return Status::kUnsupported;
}

size_t CountSet(const uint8_t* bitmap, size_t bits) {
  size_t count = 0;
  for (size_t i = 0; i < bits; ++i) count += BitmapIsSet(bitmap, i) ? 1 : 0;
  return count;
}

bool Selected(const DecodeOptions& options, size_t ordinal) {
  return options.columns.empty() || (ordinal < options.columns.size() && options.columns[ordinal]);
}

// The columns one image carries.
struct ImageColumns {
  const uint8_t* present = nullptr;  // columns-present bitmap over the TABLE_MAP columns
  size_t count = 0;                  // set bits in it: the NULL bitmap's width
};

// A PARTIAL_UPDATE_ROWS after image's value options and, under PARTIAL_JSON,
// its partial bits (one per JSON column of the table). Upstream
// start_partial_bit_reader (sql/rpl_record.cc).
Status ReadValueOptions(ByteReader* reader, size_t json_columns, const uint8_t** partial) {
  uint64_t options = 0;
  Status status = reader->ReadPacked(&options);
  if (status != Status::kOk) return status;
  if (options > kPartialJsonUpdates) return Status::kInvalidMetadata;
  *partial = nullptr;
  if ((options & kPartialJsonUpdates) == 0) return Status::kOk;
  return reader->ReadBytes(BitmapBytes(json_columns), partial);
}

// Back to a default Value, keeping the string's capacity. Must name every member of Value.
void ResetValue(Value* value) {
  value->kind = Value::Kind::kNull;
  value->type = ColumnType::kLong;
  value->i = 0;
  value->u = 0;
  value->d = 0.0;
  value->t = Temporal{};
  value->epoch_seconds = 0;
  value->microsecond = 0;
  value->fsp = 0;
  value->bytes.clear();
}

// One row image: under @p value_options the PARTIAL_UPDATE_ROWS prefix, then
// the NULL bitmap over the carried columns, then a value per carried non-NULL
// column.
Status DecodeImage(ByteReader* reader, const TableMap& table, const ImageColumns& image,
                   bool value_options, size_t json_columns, const DecodeOptions& options,
                   Row* row) {
  const size_t columns = table.columns.size();
  const uint8_t* partial = nullptr;
  Status status = Status::kOk;
  if (value_options) {
    status = ReadValueOptions(reader, json_columns, &partial);
    if (status != Status::kOk) return status;
  }
  const uint8_t* nulls = nullptr;
  status = reader->ReadBytes(BitmapBytes(image.count), &nulls);
  if (status != Status::kOk) return status;

  DecodeBudget* budget = options.budget;
  if (budget != nullptr && options.per_value_overhead != 0) {
    if (columns > budget->Remaining() / options.per_value_overhead) return Status::kLimitExceeded;
    budget->Charge(columns * options.per_value_overhead);
  }

  // A reused image is reset in place; assigning Value{} per column copies each
  // string out of line.
  if (row->size() == columns) {
    for (Value& value : *row) ResetValue(&value);
  } else {
    row->assign(columns, Value{});
  }
  size_t carried = 0;  // NULL bitmap index
  size_t json = 0;     // partial bitmap index
  for (size_t i = 0; i < columns; ++i) {
    const TableMapColumn& column = table.columns[i];
    Value& value = (*row)[i];
    // Only a STRING column can be an ENUM or SET; RealType() is not inlined here.
    value.type = column.type == ColumnType::kString ? RealType(column) : column.type;
    // Every JSON column takes a partial bit, carried or not.
    const bool is_diff =
        column.type == ColumnType::kJson && partial != nullptr && BitmapIsSet(partial, json++);
    const bool selected = Selected(options, i);
    if (!BitmapIsSet(image.present, i)) {
      if (selected) value.kind = Value::Kind::kAbsent;
      continue;
    }
    if (BitmapIsSet(nulls, carried++)) continue;
    Value* target = selected ? &value : nullptr;
    status = is_diff ? DecodeJsonDiff(reader, LimitsFor(options), target)
                     : DecodeValue(column, reader, LimitsFor(options), target);
    if (status != Status::kOk) return status;
    if (selected && budget != nullptr && !budget->Charge(value.bytes.size())) {
      return Status::kLimitExceeded;
    }
  }
  return Status::kOk;
}

// The v2 extra row info (Rows_event::Rows_event in
// libs/mysql/binlog/event/rows_event.cpp): typed entries until the first type
// code it does not know, which ends the walk.
Status ParseExtraRowInfo(const uint8_t* data, size_t size, bool update, RowsEvent* out) {
  ByteReader reader(data, size);
  while (reader.remaining() > 0) {
    uint8_t type = 0;
    Status status = reader.ReadU8(&type);
    if (status != Status::kOk) return status;
    if (type == kExtraRowInfoNdb) {
      // The length byte counts itself, the format byte and the NDB data.
      uint8_t length = 0;
      if (reader.ReadU8(&length) != Status::kOk || length < kExtraRowInfoNdbHeader ||
          length - 1u > reader.remaining()) {
        return Status::kInvalidMetadata;
      }
      status = reader.Skip(length - 1u);
    } else if (type == kExtraRowInfoPartition) {
      if (reader.remaining() < (update ? 4u : 2u)) return Status::kInvalidMetadata;
      out->has_partition_id = true;
      status = reader.ReadU16Le(&out->partition_id);
      if (status == Status::kOk && update) status = reader.ReadU16Le(&out->source_partition_id);
    } else {
      return Status::kOk;
    }
    if (status != Status::kOk) return status;
  }
  return Status::kOk;
}

Status ChargeRow(const DecodeOptions& options) {
  if (options.budget == nullptr || options.budget->Charge(options.per_row_overhead)) {
    return Status::kOk;
  }
  return Status::kLimitExceeded;
}

// Decodes every row of a rows event into @p scratch and hands each to
// @p visitor; the extra row info goes to @p info.
Status WalkRowsEvent(const EventHeader& header, const uint8_t* body, size_t size,
                     const TableMap& table, const DecodeOptions& options, RowsEvent* info,
                     RowScratch* scratch, RowVisitorFn visitor, void* ctx) {
  bool v2 = false;
  bool update = false;
  bool value_options = false;  // after images open with binlog_row_value_options
  switch (static_cast<EventType>(header.type_code)) {
    case EventType::kWriteRowsEventV1:
    case EventType::kDeleteRowsEventV1:
      break;
    case EventType::kUpdateRowsEventV1:
      update = true;
      break;
    case EventType::kWriteRowsEvent:
    case EventType::kDeleteRowsEvent:
      v2 = true;
      break;
    case EventType::kUpdateRowsEvent:
      v2 = update = true;
      break;
    case EventType::kPartialUpdateRowsEvent:
      v2 = update = value_options = true;
      break;
    default:
      return Status::kUnsupported;
  }
  if (body == nullptr) return Status::kTruncated;

  ByteReader reader(body, size);
  Status status = reader.Skip(8);  // table_id (6), flags (2)
  if (status != Status::kOk) return status;
  if (v2) {
    // The extra-data length counts its own two bytes.
    uint16_t extra = 0;
    status = reader.ReadU16Le(&extra);
    if (status != Status::kOk) return status;
    if (extra < 2) return Status::kInvalidMetadata;
    const uint8_t* extra_info = nullptr;
    status = reader.ReadBytes(extra - 2u, &extra_info);
    if (status != Status::kOk) return status;
    status = ParseExtraRowInfo(extra_info, extra - 2u, update, info);
    if (status != Status::kOk) return status;
  }

  uint64_t column_count = 0;
  status = reader.ReadPacked(&column_count);
  if (status != Status::kOk) return status;
  if (column_count == 0 || column_count > kMaxTableColumns ||
      column_count != table.columns.size()) {
    return Status::kInvalidMetadata;
  }
  const size_t columns = static_cast<size_t>(column_count);
  ImageColumns images[2];
  for (int image = 0; image < (update ? 2 : 1); ++image) {
    status = reader.ReadBytes(BitmapBytes(columns), &images[image].present);
    if (status != Status::kOk) return status;
    images[image].count = CountSet(images[image].present, columns);
  }
  const size_t json_columns = static_cast<size_t>(
      std::count_if(table.columns.begin(), table.columns.end(),
                    [](const TableMapColumn& c) { return c.type == ColumnType::kJson; }));

  Row* const first = &scratch->row;
  Row* const second = &scratch->after;
  while (reader.remaining() > 0) {
    const size_t before = reader.remaining();
    status = DecodeImage(&reader, table, images[0], false, json_columns, options, first);
    if (status != Status::kOk) return status;
    if (update) {
      status = DecodeImage(&reader, table, images[1], value_options, json_columns, options, second);
      if (status != Status::kOk) return status;
    }
    status = ChargeRow(options);
    if (status != Status::kOk) return status;
    // Images that carry no column take no bytes, so the rest cannot be rows.
    if (reader.remaining() == before) return Status::kInvalidMetadata;
    status = visitor(ctx, first, update ? second : nullptr);
    if (status != Status::kOk) return status;
  }
  return Status::kOk;
}

// Collects the visited rows into a RowsEvent.
Status CollectRow(void* ctx, Row* row, Row* after) {
  RowsEvent* data = static_cast<RowsEvent*>(ctx);
  if (after != nullptr) {
    data->updates.emplace_back(std::move(*row), std::move(*after));
  } else {
    data->rows.push_back(std::move(*row));
  }
  return Status::kOk;
}

}  // namespace

Status DecodeRowsEvent(const EventHeader& header, const uint8_t* body, size_t size,
                       const TableMap& table, const DecodeOptions& options, RowsEvent* out) {
  RowsEvent data;
  RowScratch scratch;
  const Status status =
      WalkRowsEvent(header, body, size, table, options, &data, &scratch, &CollectRow, &data);
  if (status != Status::kOk) return status;
  *out = std::move(data);
  return Status::kOk;
}

Status VisitRowsEvent(const EventHeader& header, const uint8_t* body, size_t size,
                      const TableMap& table, const DecodeOptions& options, RowScratch* scratch,
                      RowVisitorFn visitor, void* ctx) {
  RowsEvent info;
  return WalkRowsEvent(header, body, size, table, options, &info, scratch, visitor, ctx);
}

Status ParseRowsEventHeader(const EventHeader& header, const uint8_t* body, size_t size,
                            RowsEventHeader* out) {
  if (!IsRowsEvent(header.type_code)) return Status::kUnsupported;
  // Post-header: 6-byte table id, then 2-byte flags.
  if (body == nullptr || size < 8) return Status::kTruncated;
  out->table_id = ReadU48Le(body);
  out->flags = ReadU16Le(body + 6);
  return Status::kOk;
}

}  // namespace mybinlog
