// mybinlog-dump: print every event of binlog files as JSON Lines.
//
//   mybinlog-dump [--no-verify] <binlog-file>...
//
// Each file is decoded as its own stream with mybinlog::Decoder; gzip files are
// read transparently. One JSON object per event: "pos" (stream offset, the
// binlog position), "type", "disposition", "payload" for events that came out
// of a TRANSACTION_PAYLOAD, and fields for the parsed body. Row values are
// keyed by column name (or "@N" without column-name metadata) and rendered with
// FormatValue(): numbers as JSON numbers, JSON documents embedded, temporals and
// DECIMAL and YEAR as strings, binary data as uppercase hex, ENUM/SET as labels when the
// TABLE_MAP carries them. Columns left out of a MINIMAL/NOBLOB image are omitted.
// Exit status: 0 on success, 1 on a decode error, 2 on a usage or I/O error.

#include <mybinlog/mybinlog.h>
#include <zlib.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace {

void AppendHex(const std::string& bytes, std::string* out) {
  static const char kDigits[] = "0123456789ABCDEF";
  for (const char c : bytes) {
    const auto b = static_cast<unsigned char>(c);
    out->push_back(kDigits[b >> 4]);
    out->push_back(kDigits[b & 0x0F]);
  }
}

// Length of the valid UTF-8 sequence at s[i], or 0.
size_t Utf8Length(const std::string& s, size_t i) {
  const auto b = static_cast<unsigned char>(s[i]);
  size_t n = b < 0x80 ? 1 : (b >> 5) == 0x06 ? 2 : (b >> 4) == 0x0E ? 3 : (b >> 3) == 0x1E ? 4 : 0;
  if (n == 0 || i + n > s.size()) return 0;
  for (size_t k = 1; k < n; ++k) {
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
  }
  return n;
}

// JSON string; bytes that are not UTF-8 are escaped as the Latin-1 code point.
void AppendString(const std::string& s, std::string* out) {
  out->push_back('"');
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    const size_t n = Utf8Length(s, i);
    if (b == '"' || b == '\\') {
      out->push_back('\\');
      out->push_back(static_cast<char>(b));
    } else if (n == 1 && b >= 0x20) {
      out->push_back(static_cast<char>(b));
    } else if (n > 1) {
      out->append(s, i, n);
      i += n;
      continue;
    } else {
      char escape[8];
      std::snprintf(escape, sizeof(escape), "\\u%04X", b);
      out->append(escape);
    }
    ++i;
  }
  out->push_back('"');
}

void AppendKey(const char* key, std::string* out) {
  out->append(",\"");
  out->append(key);
  out->append("\":");
}

bool AppendValue(const mybinlog::Value& value, const mybinlog::TableMapColumn& column,
                 std::string* out) {
  using Kind = mybinlog::Value::Kind;
  if (value.kind == Kind::kNull) {
    out->append("null");
    return true;
  }
  std::string text;
  if (value.kind == Kind::kEnum && !column.labels.empty() &&
      mybinlog::FormatEnumLabel(value, column, &text) == mybinlog::Status::kOk) {
    AppendString(text, out);
    return true;
  }
  if (value.kind == Kind::kSet && !column.labels.empty() &&
      mybinlog::FormatSetLabels(value, column, &text) == mybinlog::Status::kOk) {
    AppendString(text, out);
    return true;
  }
  if (mybinlog::FormatValue(value, &text) != mybinlog::Status::kOk) return false;
  switch (value.kind) {
    case Kind::kInt:
    case Kind::kUInt:
    case Kind::kBit:
    case Kind::kEnum:
    case Kind::kSet:
    case Kind::kJson:
    case Kind::kJsonDiff:
      out->append(text);
      break;
    case Kind::kDouble:
      if (std::isfinite(value.d)) {
        out->append(text);
      } else {
        AppendString(text, out);
      }
      break;
    case Kind::kBytes:
      out->push_back('"');
      AppendHex(text, out);
      out->push_back('"');
      break;
    default:
      AppendString(text, out);
      break;
  }
  return true;
}

bool AppendRow(const mybinlog::Row& row, const mybinlog::TableMap& table, std::string* out) {
  out->push_back('{');
  bool first = true;
  for (size_t i = 0; i < row.size() && i < table.columns.size(); ++i) {
    if (row[i].kind == mybinlog::Value::Kind::kAbsent) continue;
    if (!first) out->push_back(',');
    first = false;
    const std::string& name = table.columns[i].name;
    AppendString(name.empty() ? "@" + std::to_string(i + 1) : name, out);
    out->push_back(':');
    if (!AppendValue(row[i], table.columns[i], out)) return false;
  }
  out->push_back('}');
  return true;
}

bool AppendRows(const mybinlog::TableRowsEvent& rows, std::string* out) {
  AppendKey("table_id", out);
  out->append(std::to_string(rows.table_id));
  if (rows.table == nullptr) return true;
  AppendKey("table", out);
  AppendString(rows.table->schema + "." + rows.table->table, out);
  if (!rows.decoded) return true;
  AppendKey("rows", out);
  out->push_back('[');
  bool first = true;
  for (const mybinlog::Row& row : rows.rows.rows) {
    if (!first) out->push_back(',');
    first = false;
    if (!AppendRow(row, *rows.table, out)) return false;
  }
  for (const auto& update : rows.rows.updates) {
    if (!first) out->push_back(',');
    first = false;
    out->append("{\"before\":");
    if (!AppendRow(update.first, *rows.table, out)) return false;
    out->append(",\"after\":");
    if (!AppendRow(update.second, *rows.table, out)) return false;
    out->push_back('}');
  }
  out->push_back(']');
  return true;
}

bool AppendBody(const mybinlog::Event& event, std::string* out) {
  const mybinlog::EventBody& body = event.body;
  if (const auto* fde = std::get_if<mybinlog::FormatDescriptionEvent>(&body)) {
    AppendKey("server_version", out);
    AppendString(fde->server_version, out);
    AppendKey("checksum", out);
    AppendString(fde->checksum_algorithm == mybinlog::ChecksumAlgorithm::kCrc32 ? "CRC32" : "NONE",
                 out);
  } else if (const auto* rotate = std::get_if<mybinlog::RotateEvent>(&body)) {
    AppendKey("next_file", out);
    AppendString(rotate->next_file, out);
    AppendKey("next_position", out);
    out->append(std::to_string(rotate->position));
  } else if (const auto* map = std::get_if<mybinlog::TableMap>(&body)) {
    AppendKey("table_id", out);
    out->append(std::to_string(map->table_id));
    AppendKey("table", out);
    AppendString(map->schema + "." + map->table, out);
  } else if (const auto* rows = std::get_if<mybinlog::TableRowsEvent>(&body)) {
    return AppendRows(*rows, out);
  } else if (const auto* query = std::get_if<mybinlog::QueryEvent>(&body)) {
    AppendKey("schema", out);
    AppendString(query->schema, out);
    AppendKey("query", out);
    AppendString(query->query, out);
  } else if (const auto* xid = std::get_if<mybinlog::XidEvent>(&body)) {
    AppendKey("xid", out);
    out->append(std::to_string(xid->xid));
  } else if (const auto* gtid = std::get_if<mybinlog::GtidEvent>(&body)) {
    AppendKey("gtid", out);
    AppendString(gtid->ToString(), out);
  } else if (const auto* previous = std::get_if<mybinlog::PreviousGtidsEvent>(&body)) {
    AppendKey("gtids", out);
    AppendString(previous->gtids.ToString(), out);
  } else if (const auto* maria_gtid = std::get_if<mybinlog::MariaDbGtidEvent>(&body)) {
    AppendKey("gtid", out);
    AppendString(maria_gtid->gtid.ToString(), out);
  } else if (const auto* list = std::get_if<mybinlog::MariaDbGtidListEvent>(&body)) {
    AppendKey("gtids", out);
    AppendString(list->gtids.ToString(), out);
  } else if (const auto* annotate = std::get_if<mybinlog::MariaDbAnnotateRowsEvent>(&body)) {
    AppendKey("query", out);
    AppendString(annotate->query, out);
  } else if (const auto* heartbeat = std::get_if<mybinlog::HeartbeatEvent>(&body)) {
    AppendKey("log_name", out);
    AppendString(heartbeat->log_name, out);
    AppendKey("log_position", out);
    out->append(std::to_string(heartbeat->position));
  } else if (const auto* unknown = std::get_if<mybinlog::UnknownEvent>(&body)) {
    AppendKey("size", out);
    out->append(std::to_string(unknown->size));
  }
  return true;
}

// Decodes one file; returns the process exit status for it.
int DumpFile(const char* path, const mybinlog::DecoderOptions& options) {
  gzFile file = gzopen(path, "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "%s: cannot open\n", path);
    return 2;
  }
  mybinlog::Decoder decoder(options);
  mybinlog::Event event;
  std::string line;
  std::vector<uint8_t> chunk(1 << 16);
  int status_code = 0;
  while (status_code == 0) {
    const int n = gzread(file, chunk.data(), static_cast<unsigned>(chunk.size()));
    if (n < 0) {
      std::fprintf(stderr, "%s: read error\n", path);
      status_code = 2;
      break;
    }
    if (n > 0) decoder.Feed(chunk.data(), static_cast<size_t>(n));
    mybinlog::Status status;
    while ((status = decoder.Next(&event)) == mybinlog::Status::kOk) {
      line = "{\"pos\":" + std::to_string(event.offset);
      AppendKey("type", &line);
      AppendString(mybinlog::EventTypeName(event.header.type_code), &line);
      AppendKey("disposition", &line);
      AppendString(mybinlog::DispositionName(event.disposition.disposition), &line);
      if (event.from_payload) line.append(",\"payload\":true");
      if (!AppendBody(event, &line)) {
        std::fprintf(stderr, "%s: event at %llu: a value has no text form\n", path,
                     static_cast<unsigned long long>(event.offset));
        status_code = 1;
        break;
      }
      line.append("}\n");
      std::fwrite(line.data(), 1, line.size(), stdout);
    }
    if (status_code != 0) break;
    if (status != mybinlog::Status::kNeedMoreData ||
        (n == 0 && decoder.Finish() != mybinlog::Status::kOk)) {
      std::fprintf(stderr, "%s: %s\n", path, decoder.last_error().message.c_str());
      status_code = 1;
    }
    if (n == 0) break;
  }
  gzclose(file);
  return status_code;
}

}  // namespace

int main(int argc, char** argv) {
  mybinlog::DecoderOptions options;
  std::vector<const char*> files;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-verify") == 0) {
      options.verify_checksum = false;
    } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      files.clear();
      break;
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: %s [--no-verify] <binlog-file>...\n", argv[0]);
    return 2;
  }
  int exit_code = 0;
  for (const char* path : files) {
    const int code = DumpFile(path, options);
    if (code > exit_code) exit_code = code;
  }
  return exit_code;
}
