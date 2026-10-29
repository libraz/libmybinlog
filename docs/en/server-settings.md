# Server settings

What a binlog contains depends on server variables: without `binlog_format=ROW` there are no row images, without `binlog_row_metadata=FULL` there are no column names. `mybinlog/server_settings.h` holds the static rules a reader can check once, from `SHOW VARIABLES`, before it starts reading. The library sends no queries; the caller supplies a lookup function.

## The rules

`kServerSettingRules` lists every rule, indexed by `RuleId`:

| `RuleId` | MySQL variable | MariaDB variable | Requirement |
|---|---|---|---|
| `kLogBin` | `log_bin` | `log_bin` | ON |
| `kGtidMode` | `gtid_mode` | - | ON |
| `kBinlogFormat` | `binlog_format` | `binlog_format` | ROW |
| `kBinlogRowImage` | `binlog_row_image` | `binlog_row_image` | FULL |
| `kNoTransactionCompression` | `binlog_transaction_compression` | - | not ON |
| `kNoPartialJson` | `binlog_row_value_options` | - | does not contain PARTIAL_JSON |
| `kNoLogBinCompress` | - | `log_bin_compress` | not ON |
| `kChecksumCrc32` | `binlog_checksum` | `binlog_checksum` | CRC32 |
| `kRowMetadataFull` | `binlog_row_metadata` | `binlog_row_metadata` | FULL |
| `kLogReplicaUpdates` | `log_replica_updates` | `log_slave_updates` | ON |

Comparisons ignore ASCII case. An "equals" rule treats a missing variable as a violation; a "not equals" or "does not contain" rule lets it pass. Each `ServerSettingRule` carries the variable names, the comparison (`RuleMatch`), its operand and a human-readable `expected` text; `GetRule()` returns one and `RuleIdName()` its name.

## Choosing the rules

The rules describe the strictest reader: full row images, no compression, no partial JSON. The decoder itself handles MINIMAL and NOBLOB images, both kinds of compression and partial JSON, so a reader picks the rules its own logic depends on. A reader that replays MINIMAL images leaves out `kBinlogRowImage`; one built with zstd leaves out `kNoTransactionCompression`.

## Evaluating

```cpp
bool LookupVariable(void* ctx, std::string_view name, std::string* value) {
  auto* variables = static_cast<std::map<std::string, std::string, std::less<>>*>(ctx);
  const auto it = variables->find(name);
  if (it == variables->end()) return false;
  *value = it->second;
  return true;
}

// variables: the result of SHOW GLOBAL VARIABLES, read by the caller.
const mybinlog::RuleId rules[] = {
    mybinlog::RuleId::kLogBin,        mybinlog::RuleId::kBinlogFormat,
    mybinlog::RuleId::kChecksumCrc32, mybinlog::RuleId::kRowMetadataFull,
    mybinlog::RuleId::kGtidMode,
};
std::vector<mybinlog::Violation> violations;
mybinlog::EvaluateServerSettings(LookupVariable, &variables, flavor, rules, std::size(rules),
                                 &violations);
for (const mybinlog::Violation& v : violations) {
  std::printf("%s: %s is %s, expected %s\n", mybinlog::RuleIdName(v.id), v.variable.c_str(),
              v.absent ? "absent" : v.observed.c_str(), v.expected);
}
```

`EvaluateServerSettings()` skips rules that do not apply to the flavor (here `kGtidMode` on MariaDB), appends one `Violation` per failed rule in the order of the ids, and returns the number appended. A `Violation` names the rule, the flavor-specific variable, the observed value and whether the variable was absent.

## Detecting the flavor

The rules and the TABLE_MAP parser need to know whether the server is MySQL or MariaDB. `mybinlog/server_flavor.h` offers two ways:

- `DetectFlavorFromVersion(version)` reads a handshake or `VERSION()` string: one containing "MariaDB" is MariaDB, anything else MySQL.
- `ClassifyFlavorProbe(query_succeeded, result_read_succeeded, error_number)` reads the outcome of `SELECT @@GLOBAL.gtid_binlog_pos`, a variable only MariaDB has. It returns MariaDB when the query and its result succeeded, MySQL when the query failed with error 1193 (unknown system variable), and `std::nullopt` for any other outcome.

The decoder needs neither: it detects the flavor from each FDE's server version, and `Decoder::flavor()` reports it.
