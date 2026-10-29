#include "mybinlog/table_map.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "bytes_internal.h"

namespace mybinlog {

namespace {

// Optional-metadata kinds this parser interprets (upstream
// Table_map_event::Optional_metadata_field_type). Every other kind is skipped.
constexpr uint8_t kSignedness = 1;
constexpr uint8_t kDefaultCharset = 2;
constexpr uint8_t kColumnCharset = 3;
constexpr uint8_t kColumnName = 4;
constexpr uint8_t kSetStrValue = 5;
constexpr uint8_t kEnumStrValue = 6;
constexpr uint8_t kEnumAndSetDefaultCharset = 10;
constexpr uint8_t kEnumAndSetColumnCharset = 11;

constexpr uint8_t kMaxLengthPrefixWidth = 4;

// Inside a block whose length the event declares, running out of bytes means
// the block disagrees with the columns, not that the event was cut short.
Status WithinBlock(Status status) {
  return status == Status::kTruncated ? Status::kInvalidMetadata : status;
}

Status ReadU16BeInto(ByteReader* reader, uint32_t* out) {
  const uint8_t* bytes = nullptr;
  const Status status = reader->ReadBytes(2, &bytes);
  if (status != Status::kOk) return status;
  *out = mybinlog::ReadU16Be(bytes);
  return Status::kOk;
}

Status ReadU8Into(ByteReader* reader, uint32_t* out) {
  uint8_t value = 0;
  const Status status = reader->ReadU8(&value);
  if (status != Status::kOk) return status;
  *out = value;
  return Status::kOk;
}

Status ReadU16LeInto(ByteReader* reader, uint32_t* out) {
  uint16_t value = 0;
  const Status status = reader->ReadU16Le(&value);
  if (status != Status::kOk) return status;
  *out = value;
  return Status::kOk;
}

// Field_typed_array::do_save_field_metadata writes the element type, then
// element metadata for exactly these element types.
Status ReadTypedArrayMeta(ByteReader* block, TableMapColumn* column) {
  uint8_t element = 0;
  Status status = block->ReadU8(&element);
  if (status != Status::kOk) return status;
  column->element_type = static_cast<ColumnType>(element);
  switch (column->element_type) {
    case ColumnType::kVarchar:
      return block->ReadU24Le(&column->meta);
    case ColumnType::kNewDecimal:
      return ReadU16BeInto(block, &column->meta);
    case ColumnType::kTime2:
    case ColumnType::kDatetime2:
      return ReadU8Into(block, &column->meta);
    case ColumnType::kLongLong:
    case ColumnType::kNewDate:
      return Status::kOk;
    default:
      return Status::kInvalidMetadata;
  }
}

// Reads one column's entry from the metadata block.
Status ReadColumnMeta(ByteReader* block, TableMapColumn* column) {
  switch (column->type) {
    case ColumnType::kDecimal:
    case ColumnType::kTiny:
    case ColumnType::kShort:
    case ColumnType::kLong:
    case ColumnType::kLongLong:
    case ColumnType::kInt24:
    case ColumnType::kYear:
    case ColumnType::kDate:
    case ColumnType::kNewDate:
    case ColumnType::kTime:
    case ColumnType::kDatetime:
    case ColumnType::kTimestamp:
      return Status::kOk;

    case ColumnType::kFloat:
    case ColumnType::kDouble:
    case ColumnType::kTimestamp2:
    case ColumnType::kDatetime2:
    case ColumnType::kTime2:
      return ReadU8Into(block, &column->meta);

    case ColumnType::kTinyBlob:
    case ColumnType::kMediumBlob:
    case ColumnType::kLongBlob:
    case ColumnType::kBlob:
    case ColumnType::kBlobCompressed:
    case ColumnType::kGeometry:
    case ColumnType::kJson:
    case ColumnType::kVector: {
      const Status status = ReadU8Into(block, &column->meta);
      if (status != Status::kOk) return status;
      // The width of the value's length prefix; upstream rejects 0 and > 4.
      if (column->meta < 1 || column->meta > kMaxLengthPrefixWidth) {
        return Status::kInvalidMetadata;
      }
      return Status::kOk;
    }

    case ColumnType::kVarchar:
    case ColumnType::kVarcharCompressed:
    case ColumnType::kBit:
      return ReadU16LeInto(block, &column->meta);

    // Field_string::do_save_field_metadata: real type first, then length.
    // VAR_STRING (MariaDB's MySQL-4.1 VARCHAR) is written by the same function.
    case ColumnType::kString:
    case ColumnType::kVarString:
    case ColumnType::kEnum:
    case ColumnType::kSet:
    case ColumnType::kNewDecimal:
      return ReadU16BeInto(block, &column->meta);

    case ColumnType::kTypedArray:
      return ReadTypedArrayMeta(block, column);
  }
  return Status::kUnsupported;
}

// Upstream is_character_type over the real type; MariaDB marks every field
// with a charset, which includes GEOMETRY. A VARCHAR typed array has VARCHAR
// as its real type.
bool IsCharacterColumn(const TableMapColumn& column, ServerFlavor flavor) {
  switch (column.type) {
    case ColumnType::kString:
      return RealType(column) == ColumnType::kString;
    case ColumnType::kVarString:
    case ColumnType::kVarchar:
    case ColumnType::kVarcharCompressed:
    case ColumnType::kTinyBlob:
    case ColumnType::kMediumBlob:
    case ColumnType::kLongBlob:
    case ColumnType::kBlob:
    case ColumnType::kBlobCompressed:
    case ColumnType::kVector:
      return true;
    case ColumnType::kGeometry:
      return flavor == ServerFlavor::kMariaDB;
    case ColumnType::kTypedArray:
      return column.element_type == ColumnType::kVarchar;
    default:
      return false;
  }
}

bool IsEnumOrSetColumn(const TableMapColumn& column) {
  const ColumnType real = RealType(column);
  return real == ColumnType::kEnum || real == ColumnType::kSet;
}

template <typename Predicate>
std::vector<TableMapColumn*> ColumnsWhere(TableMap* map, Predicate predicate) {
  std::vector<TableMapColumn*> columns;
  for (TableMapColumn& column : map->columns) {
    if (predicate(column)) columns.push_back(&column);
  }
  return columns;
}

Status ReadPackedU32(ByteReader* value, uint32_t* out) {
  uint64_t packed = 0;
  const Status status = WithinBlock(value->ReadPacked(&packed));
  if (status != Status::kOk) return status;
  if (packed > std::numeric_limits<uint32_t>::max()) return Status::kInvalidMetadata;
  *out = static_cast<uint32_t>(packed);
  return Status::kOk;
}

Status ReadLengthPrefixedString(ByteReader* value, std::string* out) {
  uint64_t length = 0;
  Status status = WithinBlock(value->ReadPacked(&length));
  if (status != Status::kOk) return status;
  if (length > value->remaining()) return Status::kInvalidMetadata;
  const uint8_t* bytes = nullptr;
  status = value->ReadBytes(static_cast<size_t>(length), &bytes);
  if (status != Status::kOk) return status;
  out->assign(reinterpret_cast<const char*>(bytes), static_cast<size_t>(length));
  return Status::kOk;
}

// SIGNEDNESS: one bit per numeric column, most significant bit first.
Status ApplySignedness(ByteReader value, TableMap* map) {
  const size_t numeric = static_cast<size_t>(
      std::count_if(map->columns.begin(), map->columns.end(),
                    [](const TableMapColumn& column) { return HasSignednessBit(column.type); }));
  if (value.remaining() != BitmapBytes(numeric)) return Status::kInvalidMetadata;
  const uint8_t* bitmap = value.position();
  size_t bit = 0;
  for (TableMapColumn& column : map->columns) {
    if (!HasSignednessBit(column.type)) continue;
    column.is_unsigned = (bitmap[bit / 8] & (0x80U >> (bit % 8))) != 0;
    ++bit;
  }
  map->has_signedness = true;
  return Status::kOk;
}

// DEFAULT_CHARSET form: the default collation, then (index, collation) pairs
// for the columns that differ, indexed within @p columns.
Status ApplyDefaultCharset(ByteReader value, const std::vector<TableMapColumn*>& columns) {
  uint32_t collation = 0;
  Status status = ReadPackedU32(&value, &collation);
  if (status != Status::kOk) return status;
  for (TableMapColumn* column : columns) {
    column->charset = collation;
    column->has_charset = true;
  }
  while (value.remaining() > 0) {
    uint32_t index = 0;
    status = ReadPackedU32(&value, &index);
    if (status != Status::kOk) return status;
    status = ReadPackedU32(&value, &collation);
    if (status != Status::kOk) return status;
    if (index >= columns.size()) return Status::kInvalidMetadata;
    columns[index]->charset = collation;
  }
  return Status::kOk;
}

// COLUMN_CHARSET form: one collation per column of @p columns.
Status ApplyColumnCharset(ByteReader value, const std::vector<TableMapColumn*>& columns) {
  for (TableMapColumn* column : columns) {
    const Status status = ReadPackedU32(&value, &column->charset);
    if (status != Status::kOk) return status;
    column->has_charset = true;
  }
  return value.remaining() == 0 ? Status::kOk : Status::kInvalidMetadata;
}

Status ApplyColumnNames(ByteReader value, TableMap* map) {
  for (TableMapColumn& column : map->columns) {
    const Status status = ReadLengthPrefixedString(&value, &column.name);
    if (status != Status::kOk) return status;
  }
  if (value.remaining() != 0) return Status::kInvalidMetadata;
  map->has_column_names = true;
  return Status::kOk;
}

// ENUM_STR_VALUE / SET_STR_VALUE: per column of the kind, a label count and
// that many length-prefixed labels.
Status ApplyLabels(ByteReader value, const std::vector<TableMapColumn*>& columns) {
  for (TableMapColumn* column : columns) {
    uint64_t count = 0;
    Status status = WithinBlock(value.ReadPacked(&count));
    if (status != Status::kOk) return status;
    // Every label costs at least its length byte.
    if (count > value.remaining()) return Status::kInvalidMetadata;
    column->labels.resize(static_cast<size_t>(count));
    for (std::string& label : column->labels) {
      status = ReadLengthPrefixedString(&value, &label);
      if (status != Status::kOk) return status;
    }
  }
  return value.remaining() == 0 ? Status::kOk : Status::kInvalidMetadata;
}

Status ApplyOptionalField(uint8_t kind, ByteReader value, ServerFlavor flavor, TableMap* map) {
  const auto is_character = [flavor](const TableMapColumn& column) {
    return IsCharacterColumn(column, flavor);
  };
  const auto has_real_type = [](ColumnType type) {
    return [type](const TableMapColumn& column) { return RealType(column) == type; };
  };
  switch (kind) {
    case kSignedness:
      return ApplySignedness(value, map);
    case kDefaultCharset:
      return ApplyDefaultCharset(value, ColumnsWhere(map, is_character));
    case kColumnCharset:
      return ApplyColumnCharset(value, ColumnsWhere(map, is_character));
    case kColumnName:
      return ApplyColumnNames(value, map);
    case kSetStrValue: {
      const Status status = ApplyLabels(value, ColumnsWhere(map, has_real_type(ColumnType::kSet)));
      if (status == Status::kOk) map->has_set_labels = true;
      return status;
    }
    case kEnumStrValue: {
      const Status status = ApplyLabels(value, ColumnsWhere(map, has_real_type(ColumnType::kEnum)));
      if (status == Status::kOk) map->has_enum_labels = true;
      return status;
    }
    case kEnumAndSetDefaultCharset:
      return ApplyDefaultCharset(value, ColumnsWhere(map, IsEnumOrSetColumn));
    case kEnumAndSetColumnCharset:
      return ApplyColumnCharset(value, ColumnsWhere(map, IsEnumOrSetColumn));
    default:
      return Status::kOk;
  }
}

// TLVs (kind byte, packed length, value) up to the end of the body.
Status ParseOptionalMetadata(ByteReader reader, ServerFlavor flavor, TableMap* map) {
  while (reader.remaining() > 0) {
    uint8_t kind = 0;
    Status status = reader.ReadU8(&kind);
    if (status != Status::kOk) return status;
    uint64_t length = 0;
    status = reader.ReadPacked(&length);
    if (status != Status::kOk) return status;
    if (length > reader.remaining()) return Status::kTruncated;
    const uint8_t* value = nullptr;
    status = reader.ReadBytes(static_cast<size_t>(length), &value);
    if (status != Status::kOk) return status;
    status = ApplyOptionalField(kind, ByteReader(value, static_cast<size_t>(length)), flavor, map);
    if (status != Status::kOk) return status;
  }
  return Status::kOk;
}

// One-byte length, the name, then a NUL.
Status ReadName(ByteReader* reader, std::string* out) {
  uint8_t length = 0;
  Status status = reader->ReadU8(&length);
  if (status != Status::kOk) return status;
  const uint8_t* bytes = nullptr;
  status = reader->ReadBytes(static_cast<size_t>(length) + 1, &bytes);
  if (status != Status::kOk) return status;
  out->assign(reinterpret_cast<const char*>(bytes), length);
  return Status::kOk;
}

}  // namespace

bool HasSignednessBit(ColumnType type) noexcept {
  switch (type) {
    case ColumnType::kTiny:
    case ColumnType::kShort:
    case ColumnType::kInt24:
    case ColumnType::kLong:
    case ColumnType::kLongLong:
    case ColumnType::kYear:
    case ColumnType::kFloat:
    case ColumnType::kDouble:
    case ColumnType::kDecimal:
    case ColumnType::kNewDecimal:
      return true;
    default:
      return false;
  }
}

ColumnType RealType(const TableMapColumn& column) noexcept {
  if (column.type == ColumnType::kString) {
    // A CHAR's byte 0 is STRING with length bits XORed into its high nibble,
    // so it never collides with ENUM or SET.
    const auto real = static_cast<ColumnType>(column.meta >> 8);
    if (real == ColumnType::kEnum || real == ColumnType::kSet) return real;
  }
  return column.type;
}

Status ParseTableMapEvent(const EventHeader& /*header*/, const uint8_t* body, size_t size,
                          ServerFlavor flavor, TableMap* out) {
  if (body == nullptr) return Status::kTruncated;
  ByteReader reader(body, size);
  TableMap map;

  Status status = reader.ReadU48Le(&map.table_id);
  if (status != Status::kOk) return status;
  status = reader.Skip(2);  // flags
  if (status != Status::kOk) return status;
  status = ReadName(&reader, &map.schema);
  if (status != Status::kOk) return status;
  status = ReadName(&reader, &map.table);
  if (status != Status::kOk) return status;

  uint64_t column_count = 0;
  status = reader.ReadPacked(&column_count);
  if (status != Status::kOk) return status;
  if (column_count == 0 || column_count > kMaxTableColumns) return Status::kInvalidMetadata;
  const size_t columns = static_cast<size_t>(column_count);

  const uint8_t* types = nullptr;
  status = reader.ReadBytes(columns, &types);
  if (status != Status::kOk) return status;

  uint64_t metadata_length = 0;
  status = reader.ReadPacked(&metadata_length);
  if (status != Status::kOk) return status;
  if (metadata_length > reader.remaining()) return Status::kTruncated;
  const uint8_t* metadata = nullptr;
  status = reader.ReadBytes(static_cast<size_t>(metadata_length), &metadata);
  if (status != Status::kOk) return status;

  map.columns.resize(columns);
  ByteReader block(metadata, static_cast<size_t>(metadata_length));
  for (size_t i = 0; i < columns; ++i) {
    map.columns[i].type = static_cast<ColumnType>(types[i]);
    status = WithinBlock(ReadColumnMeta(&block, &map.columns[i]));
    if (status != Status::kOk) return status;
  }
  // Upstream table_def requires the block to be consumed exactly.
  if (block.remaining() != 0) return Status::kInvalidMetadata;

  const uint8_t* null_bitmap = nullptr;
  status = reader.ReadBytes(BitmapBytes(columns), &null_bitmap);
  if (status != Status::kOk) return status;
  for (size_t i = 0; i < columns; ++i) map.columns[i].nullable = BitmapIsSet(null_bitmap, i);

  status = ParseOptionalMetadata(reader, flavor, &map);
  if (status != Status::kOk) return status;
  *out = std::move(map);
  return Status::kOk;
}

}  // namespace mybinlog
