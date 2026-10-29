// Temporal column values: the pairwise model in models/temporal.json, its
// mandatory rows, and wire vectors from MySQL's my_time.cc documentation and
// earlier decoders.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

struct Writer {
  Bytes bytes;
  Writer& U8(uint64_t v) { return Le(v, 1); }
  Writer& Le(uint64_t v, int n) {
    for (int i = 0; i < n; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Writer& Be(uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Writer& Raw(const Bytes& b) {
    bytes.insert(bytes.end(), b.begin(), b.end());
    return *this;
  }
};

TableMapColumn Column(ColumnType type, uint32_t meta = 0) {
  TableMapColumn column;
  column.type = type;
  column.meta = meta;
  return column;
}

Status DecodeOne(const TableMapColumn& column, const Bytes& value, Value* out) {
  TableMap table;
  table.table_id = 1;
  table.columns = {column};
  Writer w;
  w.Le(1, 6).Le(0, 2).Le(2, 2).U8(1).U8(0x01).U8(0x00).Raw(value);
  const Bytes body(w.bytes.begin(), w.bytes.end());
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kWriteRowsEvent);
  RowsEvent data;
  const Status status =
      DecodeRowsEvent(header, body.data(), body.size(), table, DecodeOptions(), &data);
  if (status != Status::kOk) return status;
  if (data.rows.size() != 1 || data.rows[0].size() != 1) {
    ADD_FAILURE() << "the value did not consume exactly its own bytes";
    return Status::kMalformedValue;
  }
  *out = data.rows[0][0];
  return Status::kOk;
}

// --- Encoders following mysys/my_time.cc, independent of the decoder ---

struct Fields {
  bool negative = false;
  int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int64_t usec = 0;
  int64_t epoch = 0;  // TIMESTAMP2 only
};

int64_t FloorShift24(int64_t v) {
  return v >= 0 ? v / (1 << 24) : -((-v + (1 << 24) - 1) / (1 << 24));
}

// Stored fraction width and scale: 1 byte of 1/100 s, 2 of 1/10000 s, or 3 of microseconds.
int FracBytes(int fsp) { return (fsp + 1) / 2; }
int64_t FracDivisor(int fsp) { return FracBytes(fsp) == 1 ? 10000 : FracBytes(fsp) == 2 ? 100 : 1; }

Bytes EncodeDate(const Fields& f) {
  return Writer().Le(static_cast<uint64_t>(f.year << 9 | f.month << 5 | f.day), 3).bytes;
}

Bytes EncodeTime(const Fields& f) {
  const int64_t hhmmss = f.hour * 10000 + f.minute * 100 + f.second;
  return Writer().Le(static_cast<uint64_t>(f.negative ? -hhmmss : hhmmss), 3).bytes;
}

Bytes EncodeDatetime(const Fields& f) {
  const uint64_t digits = static_cast<uint64_t>(
      ((((f.year * 100 + f.month) * 100 + f.day) * 100 + f.hour) * 100 + f.minute) * 100 +
      f.second);
  return Writer().Le(digits, 8).bytes;
}

Bytes EncodeYear(const Fields& f) {
  return {static_cast<uint8_t>(f.year == 0 ? 0 : f.year - 1900)};
}

Bytes EncodeTimestamp2(const Fields& f, int fsp) {
  Writer w;
  w.Be(static_cast<uint64_t>(f.epoch), 4);
  if (fsp > 0) w.Be(static_cast<uint64_t>(f.usec / FracDivisor(fsp)), FracBytes(fsp));
  return w.bytes;
}

// my_datetime_packed_to_binary.
Bytes EncodeDatetime2(const Fields& f, int fsp) {
  const int64_t ymd = ((f.year * 13 + f.month) << 5) | f.day;
  const int64_t hms = (f.hour << 12) | (f.minute << 6) | f.second;
  const int64_t packed = ((ymd << 17) | hms) * (int64_t{1} << 24) + f.usec;
  Writer w;
  w.Be(static_cast<uint64_t>(FloorShift24(packed) + 0x8000000000LL), 5);
  const int64_t frac = packed % (int64_t{1} << 24);
  if (fsp > 0) w.Be(static_cast<uint64_t>(frac / FracDivisor(fsp)), FracBytes(fsp));
  return w.bytes;
}

// my_time_packed_to_binary: the integer part is floored, the fraction keeps
// the sign of the whole value.
Bytes EncodeTime2(const Fields& f, int fsp) {
  const int64_t hms = (f.hour << 12) | (f.minute << 6) | f.second;
  const int64_t magnitude = hms * (int64_t{1} << 24) + f.usec;
  const int64_t packed = f.negative ? -magnitude : magnitude;
  Writer w;
  if (FracBytes(fsp) == 3) {
    w.Be(static_cast<uint64_t>(packed + 0x800000000000LL), 6);
    return w.bytes;
  }
  w.Be(static_cast<uint64_t>(FloorShift24(packed) + 0x800000), 3);
  const int64_t frac = packed % (int64_t{1} << 24);
  if (fsp > 0) w.Be(static_cast<uint64_t>(frac / FracDivisor(fsp)), FracBytes(fsp));
  return w.bytes;
}

// --- The pairwise model ---

enum class Type { kDate, kDatetime, kDatetime2, kTime, kTime2, kTimestamp2, kYear };
enum class Class { kZero, kPartialZero, kMin, kMax, kOutOfRange };

struct ModelRow {
  Type type;
  int fsp;
  bool negative;
  Class value_class;
};

// models/temporal.json "rows", in order.
const ModelRow kModelRows[] = {
    {Type::kDatetime2, 3, false, Class::kMax},
    {Type::kDate, 0, false, Class::kMax},
    {Type::kTime, 0, false, Class::kOutOfRange},
    {Type::kTime, 0, true, Class::kMax},
    {Type::kTime2, 3, true, Class::kMin},
    {Type::kDatetime2, 1, false, Class::kMin},
    {Type::kDatetime, 0, false, Class::kZero},
    {Type::kTimestamp2, 3, false, Class::kZero},
    {Type::kDate, 0, false, Class::kPartialZero},
    {Type::kDatetime, 0, false, Class::kMin},
    {Type::kTime2, 6, false, Class::kMax},
    {Type::kYear, 0, false, Class::kZero},
    {Type::kTimestamp2, 1, false, Class::kMax},
    {Type::kTime, 0, true, Class::kMin},
    {Type::kDate, 0, false, Class::kZero},
    {Type::kTime2, 3, true, Class::kOutOfRange},
    {Type::kTime2, 6, true, Class::kOutOfRange},
    {Type::kTime, 0, false, Class::kZero},
    {Type::kTimestamp2, 1, false, Class::kOutOfRange},
    {Type::kTime2, 1, true, Class::kOutOfRange},
    {Type::kTime2, 1, false, Class::kZero},
    {Type::kDate, 0, false, Class::kMin},
    {Type::kYear, 0, false, Class::kMax},
    {Type::kDatetime2, 6, false, Class::kZero},
    {Type::kDatetime, 0, false, Class::kOutOfRange},
    {Type::kDate, 0, false, Class::kOutOfRange},
    {Type::kTimestamp2, 6, false, Class::kMin},
    {Type::kTime2, 0, false, Class::kOutOfRange},
    {Type::kDatetime2, 0, false, Class::kOutOfRange},
    {Type::kDatetime2, 3, false, Class::kPartialZero},
    {Type::kDatetime2, 1, false, Class::kPartialZero},
    {Type::kDatetime2, 6, false, Class::kPartialZero},
    {Type::kYear, 0, false, Class::kMin},
    {Type::kTimestamp2, 0, false, Class::kZero},
    {Type::kDatetime, 0, false, Class::kMax},
    {Type::kDatetime, 0, false, Class::kPartialZero},
};

bool IsTime(Type type) { return type == Type::kTime || type == Type::kTime2; }

// Smallest and largest fraction representable at @p fsp (0 when fsp is 0).
int64_t FracUnit(int fsp) {
  int64_t unit = 1000000;
  for (int i = 0; i < fsp; ++i) unit /= 10;
  return fsp == 0 ? 0 : unit;
}
int64_t FracMax(int fsp) { return fsp == 0 ? 0 : 1000000 - FracUnit(fsp); }

/*
 * Value classes:
 *   zero          every field 0 (TIMESTAMP2 epoch 0, YEAR byte 0)
 *   partial_zero  2024-00-00 (DATE) or 2024-00-00 01:02:03 + the largest fraction
 *   min           0001-01-01 [00:00:00 + the smallest fraction]; TIME 00:00:01 or the
 *                 smallest fraction, negative TIME -838:59:59 + the largest fraction;
 *                 TIMESTAMP2 epoch 1 + the smallest fraction; YEAR 1901
 *   max           9999-12-31 [23:59:59 + the largest fraction]; TIME 838:59:59 + the
 *                 largest fraction, negative TIME -00:00:01 or minus the smallest
 *                 fraction; TIMESTAMP2 epoch 2^31-1 + the largest fraction; YEAR 2155
 *   out_of_range  DATE month 13, DATETIME hour 24, DATETIME2 minute 60, TIME minute 60,
 *                 TIME2 hour 839, TIMESTAMP2 a fraction of one second
 */
Fields FieldsFor(const ModelRow& row) {
  Fields f;
  f.negative = row.negative;
  switch (row.value_class) {
    case Class::kZero:
      break;
    case Class::kPartialZero:
      f.year = 2024;
      if (row.type != Type::kDate) {
        f.hour = 1;
        f.minute = 2;
        f.second = 3;
        f.usec = FracMax(row.fsp);
      }
      break;
    case Class::kMin:
      if (IsTime(row.type) && row.negative) {
        f.hour = 838;
        f.minute = 59;
        f.second = 59;
        f.usec = FracMax(row.fsp);
      } else if (IsTime(row.type)) {
        f.usec = FracUnit(row.fsp);
        if (row.fsp == 0) f.second = 1;
      } else if (row.type == Type::kTimestamp2) {
        f.epoch = 1;
        f.usec = FracUnit(row.fsp);
      } else if (row.type == Type::kYear) {
        f.year = 1901;
      } else {
        f.year = f.month = f.day = 1;
        f.usec = FracUnit(row.fsp);
      }
      break;
    case Class::kMax:
      if (IsTime(row.type) && row.negative) {
        f.usec = FracUnit(row.fsp);
        if (row.fsp == 0) f.second = 1;
      } else if (IsTime(row.type)) {
        f.hour = 838;
        f.minute = 59;
        f.second = 59;
        f.usec = FracMax(row.fsp);
      } else if (row.type == Type::kTimestamp2) {
        f.epoch = 2147483647;
        f.usec = FracMax(row.fsp);
      } else if (row.type == Type::kYear) {
        f.year = 2155;
      } else {
        f.year = 9999;
        f.month = 12;
        f.day = 31;
        if (row.type != Type::kDate) {
          f.hour = 23;
          f.minute = 59;
          f.second = 59;
          f.usec = FracMax(row.fsp);
        }
      }
      break;
    case Class::kOutOfRange:
      f.year = 2024;
      f.month = 1;
      f.day = 1;
      f.usec = FracUnit(row.fsp);
      if (row.type == Type::kDate) f.month = 13;
      if (row.type == Type::kDatetime) f.hour = 24;
      if (row.type == Type::kDatetime2) f.minute = 60;
      if (row.type == Type::kTime) f.minute = 60;  // 839 hours would overflow 24 bits
      if (row.type == Type::kTime2) f.hour = 839;
      if (row.type == Type::kTimestamp2) {
        f.epoch = 1;
        f.usec = 1000000;
      }
      break;
  }
  return f;
}

struct Encoded {
  TableMapColumn column;
  Bytes wire;
  Value::Kind kind;
};

Encoded Encode(const ModelRow& row, const Fields& f) {
  const uint32_t fsp = static_cast<uint32_t>(row.fsp);
  switch (row.type) {
    case Type::kDate:
      return {Column(ColumnType::kDate), EncodeDate(f), Value::Kind::kDate};
    case Type::kDatetime:
      return {Column(ColumnType::kDatetime), EncodeDatetime(f), Value::Kind::kDateTime};
    case Type::kDatetime2:
      return {Column(ColumnType::kDatetime2, fsp), EncodeDatetime2(f, row.fsp),
              Value::Kind::kDateTime};
    case Type::kTime:
      return {Column(ColumnType::kTime), EncodeTime(f), Value::Kind::kTime};
    case Type::kTime2:
      return {Column(ColumnType::kTime2, fsp), EncodeTime2(f, row.fsp), Value::Kind::kTime};
    case Type::kTimestamp2:
      return {Column(ColumnType::kTimestamp2, fsp), EncodeTimestamp2(f, row.fsp),
              Value::Kind::kTimestamp};
    case Type::kYear:
      return {Column(ColumnType::kYear), EncodeYear(f), Value::Kind::kYear};
  }
  return {};
}

TEST(CodecRowsTemporal, PairwiseModel) {
  size_t index = 0;
  for (const ModelRow& row : kModelRows) {
    SCOPED_TRACE(testing::Message() << "model row " << index++);
    const Fields f = FieldsFor(row);
    const Encoded encoded = Encode(row, f);
    Value value;
    const Status status = DecodeOne(encoded.column, encoded.wire, &value);
    if (row.value_class == Class::kOutOfRange) {
      EXPECT_EQ(status, Status::kMalformedValue);
      continue;
    }
    ASSERT_EQ(status, Status::kOk);
    ASSERT_EQ(value.kind, encoded.kind);
    EXPECT_EQ(value.type, encoded.column.type);
    if (row.type == Type::kYear) {
      EXPECT_EQ(value.u, static_cast<uint64_t>(f.year));
    } else if (row.type == Type::kTimestamp2) {
      EXPECT_EQ(value.epoch_seconds, f.epoch);
      EXPECT_EQ(value.microsecond, static_cast<uint32_t>(f.usec));
      EXPECT_EQ(value.fsp, row.fsp);
    } else {
      EXPECT_EQ(value.t.negative, f.negative);
      EXPECT_EQ(value.t.year, f.year);
      EXPECT_EQ(value.t.month, f.month);
      EXPECT_EQ(value.t.day, f.day);
      EXPECT_EQ(value.t.hour, f.hour);
      EXPECT_EQ(value.t.minute, f.minute);
      EXPECT_EQ(value.t.second, f.second);
      EXPECT_EQ(value.t.microsecond, static_cast<uint32_t>(f.usec));
      EXPECT_EQ(value.t.fsp, row.fsp);
    }
  }
  EXPECT_EQ(index, 36u);
}

// --- Mandatory rows ---

TEST(CodecRowsTemporal, Time2NegativeWithFraction) {
  struct Case {
    uint32_t fsp;
    uint32_t micros;
  };
  const Case cases[] = {{1, 100000}, {2, 120000}, {3, 123000},
                        {4, 123400}, {5, 123450}, {6, 123456}};
  // -05:15:30.fraction, stored as my_time_packed_to_binary documents.
  const int64_t hms = (int64_t{5} << 12) | (15 << 6) | 30;
  for (const Case& c : cases) {
    SCOPED_TRACE(c.fsp);
    const int frac_bytes = static_cast<int>((c.fsp + 1) / 2);
    const int64_t complement = int64_t{1} << (frac_bytes * 8);
    const int64_t divisor = frac_bytes == 1 ? 10000 : frac_bytes == 2 ? 100 : 1;
    Writer w;
    if (frac_bytes == 3) {
      w.Be(static_cast<uint64_t>(-(hms * (int64_t{1} << 24) + c.micros) + 0x800000000000LL), 6);
    } else {
      w.Be(static_cast<uint64_t>(-hms - 1 + 0x800000), 3);
      w.Be(static_cast<uint64_t>(complement - c.micros / divisor), frac_bytes);
    }
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kTime2, c.fsp), w.bytes, &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kTime);
    EXPECT_TRUE(value.t.negative);
    EXPECT_EQ(value.t.hour, 5);
    EXPECT_EQ(value.t.minute, 15);
    EXPECT_EQ(value.t.second, 30);
    EXPECT_EQ(value.t.microsecond, c.micros);
    EXPECT_EQ(value.t.fsp, c.fsp);
  }

  // Disk vectors from my_time_packed_from_binary's documentation (fsp 2).
  struct Disk {
    uint32_t intpart;
    uint8_t frac;
    uint8_t second;
    uint32_t micros;
  };
  const Disk disks[] = {
      {0x7FFFFF, 0x9D, 0, 990000},  // -00:00:00.99
      {0x7FFFFF, 0xFF, 0, 10000},   // -00:00:00.01
      {0x7FFFFE, 0xFF, 1, 10000},   // -00:00:01.01, the integer part borrows
      {0x7FFFFF, 0x00, 1, 0},       // -00:00:01.00, no complement for a zero fraction
  };
  for (const Disk& d : disks) {
    SCOPED_TRACE(testing::Message() << std::hex << d.intpart << "." << int{d.frac});
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kTime2, 2), Writer().Be(d.intpart, 3).U8(d.frac).bytes,
                        &value),
              Status::kOk);
    EXPECT_TRUE(value.t.negative);
    EXPECT_EQ(value.t.hour, 0);
    EXPECT_EQ(value.t.minute, 0);
    EXPECT_EQ(value.t.second, d.second);
    EXPECT_EQ(value.t.microsecond, d.micros);
  }
}

TEST(CodecRowsTemporal, Datetime2ZeroMonthDayConsumesTheFraction) {
  // '2024-00-00 00:00:00.5' in DATETIME(6), then an INT that only lines up if
  // the three fraction bytes were consumed.
  TableMap table;
  table.table_id = 1;
  table.columns = {Column(ColumnType::kDatetime2, 6), Column(ColumnType::kLong)};
  Fields f;
  f.year = 2024;
  f.usec = 500000;
  Writer w;
  w.Le(1, 6).Le(0, 2).Le(2, 2).U8(2).U8(0x03).U8(0x00).Raw(EncodeDatetime2(f, 6)).Le(42, 4);
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kWriteRowsEvent);
  RowsEvent data;
  ASSERT_EQ(DecodeRowsEvent(header, w.bytes.data(), w.bytes.size(), table, DecodeOptions(), &data),
            Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  const Value& dt = data.rows[0][0];
  EXPECT_EQ(dt.kind, Value::Kind::kDateTime);
  EXPECT_EQ(dt.t.year, 2024);
  EXPECT_EQ(dt.t.month, 0);
  EXPECT_EQ(dt.t.day, 0);
  EXPECT_EQ(dt.t.microsecond, 500000u);
  EXPECT_EQ(data.rows[0][1].i, 42);

  // The same at every fraction width, all fields zero.
  for (uint32_t fsp = 1; fsp <= 6; ++fsp) {
    SCOPED_TRACE(fsp);
    table.columns[0].meta = fsp;
    Fields zero;
    zero.usec = static_cast<int64_t>(FracUnit(static_cast<int>(fsp)));
    Writer z;
    z.Le(1, 6).Le(0, 2).Le(2, 2).U8(2).U8(0x03).U8(0x00);
    z.Raw(EncodeDatetime2(zero, static_cast<int>(fsp))).Le(7, 4);
    ASSERT_EQ(
        DecodeRowsEvent(header, z.bytes.data(), z.bytes.size(), table, DecodeOptions(), &data),
        Status::kOk);
    ASSERT_EQ(data.rows.size(), 1u);
    EXPECT_EQ(data.rows[0][0].t.microsecond, static_cast<uint32_t>(zero.usec));
    EXPECT_EQ(data.rows[0][1].i, 7);
  }
}

// --- Wire vectors ---

Bytes Datetime2Bytes(int year, int month, int day, int hour, int minute, int second) {
  Fields f;
  f.year = year;
  f.month = month;
  f.day = day;
  f.hour = hour;
  f.minute = minute;
  f.second = second;
  return EncodeDatetime2(f, 0);
}

TEST(CodecRowsTemporal, Datetime2Fractions) {
  struct Case {
    uint32_t fsp;
    Bytes fraction;
    uint32_t micros;
  };
  // MySQL stores the same scaled value within each fsp pair.
  const Case cases[] = {
      {0, {}, 0},
      {1, {50}, 500000},
      {2, {12}, 120000},
      {2, {99}, 990000},
      {3, Writer().Be(1230, 2).bytes, 123000},
      {4, Writer().Be(5678, 2).bytes, 567800},
      {5, Writer().Be(123450, 3).bytes, 123450},
      {6, Writer().Be(123456, 3).bytes, 123456},
      {6, Writer().Be(999999, 3).bytes, 999999},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(testing::Message() << c.fsp << " " << c.micros);
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kDatetime2, c.fsp),
                        Writer().Raw(Datetime2Bytes(2024, 3, 15, 10, 30, 45)).Raw(c.fraction).bytes,
                        &value),
              Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kDateTime);
    EXPECT_EQ(value.t.year, 2024);
    EXPECT_EQ(value.t.month, 3);
    EXPECT_EQ(value.t.day, 15);
    EXPECT_EQ(value.t.hour, 10);
    EXPECT_EQ(value.t.minute, 30);
    EXPECT_EQ(value.t.second, 45);
    EXPECT_EQ(value.t.microsecond, c.micros);
    EXPECT_EQ(value.t.fsp, c.fsp);
  }
}

TEST(CodecRowsTemporal, Datetime2OutOfRangeValuesAreMalformed) {
  Value value;
  // DATETIMEF_INT_OFS - 1: a negative packed value; no DATETIME is negative.
  EXPECT_EQ(DecodeOne(Column(ColumnType::kDatetime2, 0), {0x7F, 0xFF, 0xFF, 0xFF, 0xFF}, &value),
            Status::kMalformedValue);
  // MES rendered this as "-2024-03-15 10:30:45.500000"; it is now refused.
  {
    const int64_t ymd = ((int64_t{2024} * 13 + 3) << 5) | 15;
    const int64_t hms = (int64_t{10} << 12) | (30 << 6) | 45;
    const int64_t packed = -(((ymd << 17) | hms) * (int64_t{1} << 24) + 500000);
    const int64_t intpart = FloorShift24(packed);
    const int64_t frac = packed - intpart * (int64_t{1} << 24);
    EXPECT_EQ(DecodeOne(Column(ColumnType::kDatetime2, 6),
                        Writer()
                            .Be(static_cast<uint64_t>(intpart + 0x8000000000LL), 5)
                            .Be(static_cast<uint64_t>(frac), 3)
                            .bytes,
                        &value),
              Status::kMalformedValue);
  }
  // fsp 2 stores hundredths: 100 is a whole second.
  EXPECT_EQ(DecodeOne(Column(ColumnType::kDatetime2, 2),
                      Writer().Raw(Datetime2Bytes(2000, 1, 1, 0, 0, 0)).U8(100).bytes, &value),
            Status::kMalformedValue);
  // Year 10000.
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kDatetime2, 0), Datetime2Bytes(10000, 1, 1, 0, 0, 0), &value),
      Status::kMalformedValue);
  // Hour 24.
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kDatetime2, 0), Datetime2Bytes(2024, 1, 1, 24, 0, 0), &value),
      Status::kMalformedValue);
}

TEST(CodecRowsTemporal, FspAboveSixIsInvalidMetadata) {
  for (ColumnType type : {ColumnType::kDatetime2, ColumnType::kTime2, ColumnType::kTimestamp2}) {
    SCOPED_TRACE(static_cast<int>(type));
    Value value;
    EXPECT_EQ(DecodeOne(Column(type, 7), Bytes(16, 0x80), &value), Status::kInvalidMetadata);
  }
}

TEST(CodecRowsTemporal, Time2Values) {
  struct Case {
    uint32_t fsp;
    Bytes wire;
    bool negative;
    uint16_t hour;
    uint8_t minute, second;
    uint32_t micros;
  };
  const Case cases[] = {
      {0, Writer().Be(0x800000 + ((10 << 12) | (30 << 6) | 45), 3).bytes, false, 10, 30, 45, 0},
      {0, Writer().Be(static_cast<uint64_t>(0x800000 - ((5 << 12) | (15 << 6) | 30)), 3).bytes,
       true, 5, 15, 30, 0},
      {3, Writer().Be(0x800000 + ((10 << 12) | (30 << 6) | 45), 3).Be(5000, 2).bytes, false, 10, 30,
       45, 500000},
      // fsp 5 stores microseconds: 123450.
      {5, Writer().Be(0x800000 + ((10 << 12) | (20 << 6) | 30), 3).Be(0x01E23A, 3).bytes, false, 10,
       20, 30, 123450},
      {0, Writer().Be(0x800000 + ((838 << 12) | (59 << 6) | 59), 3).bytes, false, 838, 59, 59, 0},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(testing::Message() << c.hour << ":" << int{c.minute} << ":" << int{c.second});
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kTime2, c.fsp), c.wire, &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kTime);
    EXPECT_EQ(value.t.negative, c.negative);
    EXPECT_EQ(value.t.hour, c.hour);
    EXPECT_EQ(value.t.minute, c.minute);
    EXPECT_EQ(value.t.second, c.second);
    EXPECT_EQ(value.t.microsecond, c.micros);
  }
  // Hour 1100 fits the 10-bit field; it is refused, not masked.
  Value value;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kTime2, 0), Writer().Be(0x800000 + (1100 << 12), 3).bytes,
                      &value),
            Status::kMalformedValue);
  // fsp 2 with a stored 255 hundredths on a positive value.
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kTime2, 2), Writer().Be(0x800001, 3).U8(0xFF).bytes, &value),
      Status::kMalformedValue);
}

TEST(CodecRowsTemporal, OldTimeIsASignedHhmmss) {
  struct Case {
    Bytes wire;
    bool negative;
    uint16_t hour;
    uint8_t minute, second;
  };
  const Case cases[] = {
      {Writer().Le(103045, 3).bytes, false, 10, 30, 45},
      {{0xC0, 0x1D, 0xFE}, true, 12, 34, 56},   // -123456
      {{0x59, 0x0A, 0x80}, true, 838, 59, 59},  // -8385959
      {{0x40, 0xE2, 0x01}, false, 12, 34, 56},  // 123456
      {Writer().Le(static_cast<uint32_t>(-13000) & 0xFFFFFF, 3).bytes, true, 1, 30, 0},
      {Writer().Le(static_cast<uint32_t>(-530) & 0xFFFFFF, 3).bytes, true, 0, 5, 30},
  };
  for (const Case& c : cases) {
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kTime), c.wire, &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kTime);
    EXPECT_EQ(value.t.negative, c.negative);
    EXPECT_EQ(value.t.hour, c.hour);
    EXPECT_EQ(value.t.minute, c.minute);
    EXPECT_EQ(value.t.second, c.second);
  }
  Value value;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kTime), {0xD0, 0x13, 0xFE}, &value),  // -12:60:00
            Status::kMalformedValue);
  EXPECT_EQ(DecodeOne(Column(ColumnType::kTime), {0x00, 0x00, 0x80}, &value),  // -8388608
            Status::kMalformedValue);
}

TEST(CodecRowsTemporal, LegacyDateDatetimeTimestampAndYear) {
  Value value;
  ASSERT_EQ(DecodeOne(Column(ColumnType::kDate), Writer().Le((2024 << 9) | (3 << 5) | 15, 3).bytes,
                      &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kDate);
  EXPECT_EQ(value.t.year, 2024);
  EXPECT_EQ(value.t.month, 3);
  EXPECT_EQ(value.t.day, 15);

  // NEWDATE has DATE's layout (upstream log_event_print_value).
  ASSERT_EQ(DecodeOne(Column(ColumnType::kNewDate),
                      Writer().Le((2024 << 9) | (2 << 5) | 29, 3).bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kDate);
  EXPECT_EQ(value.type, ColumnType::kNewDate);
  EXPECT_EQ(value.t.day, 29);

  ASSERT_EQ(
      DecodeOne(Column(ColumnType::kDatetime), Writer().Le(20240315103045ULL, 8).bytes, &value),
      Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kDateTime);
  EXPECT_EQ(value.t.year, 2024);
  EXPECT_EQ(value.t.month, 3);
  EXPECT_EQ(value.t.day, 15);
  EXPECT_EQ(value.t.hour, 10);
  EXPECT_EQ(value.t.minute, 30);
  EXPECT_EQ(value.t.second, 45);
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kDatetime), Writer().Le(20241315103045ULL, 8).bytes, &value),
      Status::kMalformedValue);
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kDatetime), Writer().Le(20240315106045ULL, 8).bytes, &value),
      Status::kMalformedValue);

  ASSERT_EQ(DecodeOne(Column(ColumnType::kTimestamp), Writer().Le(1710502245, 4).bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kTimestamp);
  EXPECT_EQ(value.epoch_seconds, 1710502245);
  EXPECT_EQ(value.fsp, 0);

  // DATE month 13 and day 32 do not exist.
  EXPECT_EQ(DecodeOne(Column(ColumnType::kDate), Writer().Le((2024 << 9) | (13 << 5) | 1, 3).bytes,
                      &value),
            Status::kMalformedValue);
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kDate), Writer().Le(10000 << 9 | (1 << 5) | 1, 3).bytes, &value),
      Status::kMalformedValue);

  const struct {
    uint8_t wire;
    uint64_t year;
  } years[] = {{0, 0}, {1, 1901}, {100, 2000}, {155, 2055}, {255, 2155}};
  for (const auto& y : years) {
    ASSERT_EQ(DecodeOne(Column(ColumnType::kYear), {y.wire}, &value), Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kYear);
    EXPECT_EQ(value.u, y.year);
  }
}

TEST(CodecRowsTemporal, Timestamp2) {
  struct Case {
    uint32_t fsp;
    Bytes fraction;
    uint32_t micros;
  };
  const Case cases[] = {
      {0, {}, 0},
      {1, {50}, 500000},
      {1, {0x0A}, 100000},
      {4, Writer().Be(1234, 2).bytes, 123400},
      {6, Writer().Be(123456, 3).bytes, 123456},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.micros);
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kTimestamp2, c.fsp),
                        Writer().Be(1710502245, 4).Raw(c.fraction).bytes, &value),
              Status::kOk);
    EXPECT_EQ(value.kind, Value::Kind::kTimestamp);
    EXPECT_EQ(value.epoch_seconds, 1710502245);
    EXPECT_EQ(value.microsecond, c.micros);
    EXPECT_EQ(value.fsp, c.fsp);
  }
  // Wire 0 is the zero timestamp and stays 0 ...
  Value value;
  ASSERT_EQ(DecodeOne(Column(ColumnType::kTimestamp2, 6), Bytes(7, 0), &value), Status::kOk);
  EXPECT_EQ(value.epoch_seconds, 0);
  EXPECT_EQ(value.microsecond, 0u);
  // ... but nothing lies inside its first second.
  Bytes zero_with_fraction(7, 0);
  zero_with_fraction.back() = 1;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kTimestamp2, 6), zero_with_fraction, &value),
            Status::kMalformedValue);
}

}  // namespace
}  // namespace mybinlog
