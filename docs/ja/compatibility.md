# 対応状況

ライブラリの対象は MySQL 8.4 と 9.x、MariaDB 10.11 以降です。下の表は、テストしているサーバーの版と binlog の設定の一覧です。リポジトリには、それぞれの設定で動かした実サーバーから採取した binlog ファイルがあります（`tests/fixtures/matrix`）。

表は、採取ごとの `meta.json` から生成しています。手で編集せず、次のコマンドで再生成してください。

```sh
python3 tools/gen_compat_table.py
```

`python3 tools/gen_compat_table.py --check` は何も書き込まず、ページが古いときに 0 以外で終了します。

## サーバーの版

1 行がサーバーの版 1 つで、その採取が網羅する値を示します。「設定数」は採取の数です。

<!-- compat:summary:begin -->
| 種類 | 版 | サーバーの版 | 設定数 | チェックサム | 行イメージ | 行メタデータ | 圧縮 | PARTIAL_JSON |
|---|---|---|---|---|---|---|---|---|
| MySQL | 8.4 | `8.4.13` | 6 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zstd | OFF, ON |
| MySQL | 9.7 | `9.7.4` | 5 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zstd | OFF, ON |
| MariaDB | 10.11 | `10.11.19-MariaDB-ubu2204-log` | 4 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 11.4 | `11.4.13-MariaDB-ubu2404-log` | 3 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 11.8 | `11.8.9-MariaDB-ubu2404-log` | 4 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 12.3 | `12.3.3-MariaDB-ubu2404-log` | 4 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
| MariaDB | 13.0 | `13.0.2-MariaDB-ubu2604-log` | 3 | NONE, CRC32 | FULL, MINIMAL, NOBLOB | MINIMAL, FULL | OFF, zlib | - |
<!-- compat:summary:end -->

## 設定

採取ごとに 1 行です。圧縮は、MySQL の `binlog_transaction_compression=ON` なら `zstd`、MariaDB の `log_bin_compress=ON` なら `zlib` です。PARTIAL_JSON は `binlog_row_value_options=PARTIAL_JSON` で、MySQL にだけあります。

<!-- compat:configs:begin -->
| 種類 | 版 | チェックサム | 行イメージ | 行メタデータ | 圧縮 | PARTIAL_JSON |
|---|---|---|---|---|---|---|
| MySQL | 8.4 | NONE | FULL | MINIMAL | OFF | OFF |
| MySQL | 8.4 | NONE | FULL | FULL | OFF | ON |
| MySQL | 8.4 | NONE | MINIMAL | MINIMAL | zstd | OFF |
| MySQL | 8.4 | NONE | MINIMAL | FULL | zstd | ON |
| MySQL | 8.4 | NONE | NOBLOB | MINIMAL | OFF | ON |
| MySQL | 8.4 | CRC32 | FULL | FULL | OFF | OFF |
| MySQL | 9.7 | NONE | FULL | MINIMAL | zstd | OFF |
| MySQL | 9.7 | NONE | NOBLOB | FULL | zstd | OFF |
| MySQL | 9.7 | CRC32 | FULL | FULL | OFF | OFF |
| MySQL | 9.7 | CRC32 | MINIMAL | MINIMAL | OFF | ON |
| MySQL | 9.7 | CRC32 | NOBLOB | MINIMAL | zstd | ON |
| MariaDB | 10.11 | NONE | FULL | MINIMAL | OFF | - |
| MariaDB | 10.11 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 10.11 | CRC32 | MINIMAL | FULL | OFF | - |
| MariaDB | 10.11 | CRC32 | NOBLOB | FULL | zlib | - |
| MariaDB | 11.4 | NONE | MINIMAL | FULL | zlib | - |
| MariaDB | 11.4 | NONE | NOBLOB | MINIMAL | OFF | - |
| MariaDB | 11.4 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 11.8 | NONE | MINIMAL | FULL | zlib | - |
| MariaDB | 11.8 | CRC32 | FULL | MINIMAL | OFF | - |
| MariaDB | 11.8 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 11.8 | CRC32 | NOBLOB | FULL | OFF | - |
| MariaDB | 12.3 | NONE | FULL | MINIMAL | zlib | - |
| MariaDB | 12.3 | CRC32 | FULL | FULL | OFF | - |
| MariaDB | 12.3 | CRC32 | MINIMAL | MINIMAL | OFF | - |
| MariaDB | 12.3 | CRC32 | NOBLOB | FULL | zlib | - |
| MariaDB | 13.0 | NONE | MINIMAL | MINIMAL | OFF | - |
| MariaDB | 13.0 | NONE | NOBLOB | MINIMAL | zlib | - |
| MariaDB | 13.0 | CRC32 | FULL | FULL | OFF | - |
<!-- compat:configs:end -->

設定は、チェックサム、行イメージ、行メタデータ、圧縮、PARTIAL_JSON、版について値の全ペアを網羅する組み合わせに、版ごとに CRC32、FULL の行イメージ、FULL の行メタデータ、圧縮なしの採取を 1 つ加えたものです。どの値の組も、いずれかの採取に現れます。すべての組み合わせが現れるわけではありません。

## テストで確かめていること

どの採取でも同じワークロードを実行します。全幅の整数の境界値、DECIMAL・FLOAT・DOUBLE の端の値、ゼロの日付と全精度の小数秒を含む全 temporal 型、複数の文字セットの文字データ、全幅の TEXT と BLOB、ENUM と SET、JSON（64 KB を超える文書を含む）、空間型、サーバーにあれば VECTOR、MariaDB の COMPRESSED 列、1 行の変更から多数の行イベントに分かれる大量のトランザクションまでの文です。MySQL ではタグ付きの GTID も含みます。

テストスイートは、全採取の全行イベントを、圧縮イベントは展開してからデコードします。そして、MINIMAL と NOBLOB のイメージではイメージにない列が現れ、FULL では現れないこと、PARTIAL_JSON の設定では JSON の差分が現れることを確かめます。圧縮なしの MySQL の設定では、部分更新を適用した JSON 列のテキストを、トランザクションごとにサーバー自身の `CAST(j AS CHAR)` と比べます。これとは別に、MySQL 8.4、MySQL 9.7、MariaDB 11.8 の fixture で、デコードした各値を書き込んだ SQL と照合しています。

## テスト範囲の外

- ほかのサーバーの版はテストしていません。対応するイベントのワイヤ形式は上の版の間で安定していますが、表にない版については採取による裏付けがありません。
- ステートメントベースと混合のログは、文のテキストを持つ QUERY イベントだけを生み、行イメージを持ちません。ライブラリは QUERY イベントを解析しますが、文は解釈しません。
- 暗号化された binlog ファイルは復号しません。サーバーはレプリカに送る前に復号するので、レプリケーションストリームは読めますが、ディスク上のファイルは読めません。
- MySQL 5.6.1 や MariaDB 5.3.0 より古いサーバーはチェックサムを書きません。チェックサムの処理はそれを認識しますが、それ以外はテストしていません。
