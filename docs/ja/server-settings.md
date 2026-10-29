# サーバー設定

binlog の中身はサーバー変数で決まります。`binlog_format=ROW` でなければ行イメージはなく、`binlog_row_metadata=FULL` でなければ列名はありません。`mybinlog/server_settings.h` は、読み始める前に `SHOW VARIABLES` から 1 回だけ確かめられる静的な規則を持ちます。ライブラリはクエリを送らず、呼び出し側が参照用の関数を渡します。

## 規則

`kServerSettingRules` が全規則を `RuleId` の順に並べています。

| `RuleId` | MySQL の変数 | MariaDB の変数 | 要件 |
|---|---|---|---|
| `kLogBin` | `log_bin` | `log_bin` | ON |
| `kGtidMode` | `gtid_mode` | - | ON |
| `kBinlogFormat` | `binlog_format` | `binlog_format` | ROW |
| `kBinlogRowImage` | `binlog_row_image` | `binlog_row_image` | FULL |
| `kNoTransactionCompression` | `binlog_transaction_compression` | - | ON でない |
| `kNoPartialJson` | `binlog_row_value_options` | - | PARTIAL_JSON を含まない |
| `kNoLogBinCompress` | - | `log_bin_compress` | ON でない |
| `kChecksumCrc32` | `binlog_checksum` | `binlog_checksum` | CRC32 |
| `kRowMetadataFull` | `binlog_row_metadata` | `binlog_row_metadata` | FULL |
| `kLogReplicaUpdates` | `log_replica_updates` | `log_slave_updates` | ON |

比較は ASCII の大文字・小文字を区別しません。「等しい」規則は変数がなければ違反とし、「等しくない」「含まない」規則は変数がなければ通します。各 `ServerSettingRule` は変数名、比較の方法（`RuleMatch`）、比較対象の値、人が読む `expected` のテキストを持ちます。`GetRule()` は規則を 1 つ返し、`RuleIdName()` はその名前を返します。

## 規則の選び方

規則は最も厳しい読み手を想定しています。全列の行イメージ、圧縮なし、部分 JSON なしです。デコーダ自体は MINIMAL と NOBLOB のイメージ、両方の圧縮、部分 JSON を扱えるので、読み手は自分のロジックが依存する規則を選びます。MINIMAL のイメージを再生する読み手は `kBinlogRowImage` を外し、zstd 付きでビルドした読み手は `kNoTransactionCompression` を外します。

## 評価

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

`EvaluateServerSettings()` はサーバーの種類に当てはまらない規則（この例では MariaDB での `kGtidMode`）を飛ばし、満たされない規則ごとに `Violation` を ID の順に追加し、追加した数を返します。`Violation` は規則、種類に応じた変数名、観測した値、変数がなかったかどうかを示します。

## サーバーの種類の判定

規則と TABLE_MAP のパーサは、サーバーが MySQL か MariaDB かを知る必要があります。`mybinlog/server_flavor.h` に 2 つの方法があります。

- `DetectFlavorFromVersion(version)` はハンドシェイクか `VERSION()` の文字列を読みます。"MariaDB" を含めば MariaDB、それ以外は MySQL です。
- `ClassifyFlavorProbe(query_succeeded, result_read_succeeded, error_number)` は、MariaDB にだけある変数を問い合わせる `SELECT @@GLOBAL.gtid_binlog_pos` の結果を読みます。クエリと結果の読み取りが成功すれば MariaDB、エラー 1193（未知のシステム変数）で失敗すれば MySQL、それ以外は `std::nullopt` を返します。

デコーダはどちらも必要としません。各 FDE のサーバー版から種類を判定し、`Decoder::flavor()` で報告します。
