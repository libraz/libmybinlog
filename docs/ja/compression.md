# 圧縮イベント

どちらのサーバーも binlog を圧縮できますが、方法が異なります。デコーダは既定で両方を展開し、中のイベントは圧縮されずに書かれた場合と同じ形で出てきます。

| サーバーの設定 | サーバーが書くもの | 方式 |
|---|---|---|
| MariaDB の `log_bin_compress=ON` | `log_bin_compress_min_len` を超える QUERY と行イベントを型 165-171 として | zlib |
| MySQL の `binlog_transaction_compression=ON` | トランザクションごとに 1 つの TRANSACTION_PAYLOAD イベント（型 40）で包む | zstd |
| MariaDB の列属性 `COMPRESSED` | VARCHAR、TEXT、BLOB の個々の値 | zlib |

## MariaDB の圧縮イベント

MariaDB の圧縮イベントは、先頭のフィールドを圧縮せずに残し、後ろの部分だけを圧縮します。QUERY_COMPRESSED (165) では文のテキスト、圧縮された行イベント (166-171) では行イメージです。デコーダは非圧縮のイベントを組み立て直すので、イベントは `QueryEvent` か `TableRowsEvent` として届きます。その `header.type_code` は非圧縮の型（QUERY、または WRITE/UPDATE/DELETE_ROWS の v1 か v2）で、扱いも非圧縮の型のものです。

## MySQL のトランザクションペイロード

TRANSACTION_PAYLOAD イベントは、トランザクションのイベント全体を連続して運びます。zstd で圧縮されているか、圧縮の種類が NONE なら書かれたままです。デコーダはペイロードを展開し、中のイベントを 1 回の `Next()` につき 1 つずつ順に返します。各イベントは `Event::from_payload` が立ち、`Event::offset` はペイロードイベントのオフセットです。ペイロードイベント自体は返りません。

中のイベントは、ファイルの方式にかかわらずチェックサムのトレーラを持ちません。サーバーがトレーラなしで書くためです。

## zstd はビルドオプション

zstd 対応は CMake のオプション `MYBINLOG_WITH_ZSTD` で決まります。libmybinlog をトップレベルのプロジェクトとしてビルドすると ON、別のプロジェクトが取り込むと OFF です。zstd がないと圧縮されたトランザクションペイロードは `kUnsupported` で失敗し、エラーは残り続けるので、デコーダはそこで止まります。

実行時には `HasZstdSupport()` で確かめます。たとえば `binlog_transaction_compression=ON` のサーバーに接続する前です。

```cpp
if (!mybinlog::HasZstdSupport()) {
  // this build cannot expand MySQL compressed transactions
}
```

MariaDB の圧縮に必要なのは zlib だけで、zlib は常にリンクされます。

## 展開を無効にする

`DecoderOptions::expand_compressed = false` にすると、MariaDB の圧縮イベントと TRANSACTION_PAYLOAD は、生の本体を持ち扱いが `kCompressed` の `UnknownEvent` として届きます。中身を見ずにイベントを転送する読み手は、これで展開の負荷を省けます。

## 圧縮された列

`COMPRESSED` と宣言された MariaDB の列は、通常の行イベントの中に圧縮された値を持ちます。TABLE_MAP の型は `kVarcharCompressed` か `kBlobCompressed` で、行デコーダは展開したバイト列を `kText` か `kBytes` で返します。値 1 つの展開は `DecodeOptions::max_decompressed_bytes`（既定 64 MiB）で制限され、行の予算に計上されます。[行](rows.md)を参照してください。

## 上限

| 上限 | 既定値 | 対象 |
|---|---|---|
| `DecoderOptions::max_decompressed_size` | 1 GiB | 展開した MariaDB イベントの本体 1 つ、または展開したトランザクションペイロード 1 つ。 |
| `DecodeOptions::max_decompressed_bytes` | 64 MiB | MariaDB の COMPRESSED 列の値 1 つ。 |

これを超える展開は `kLimitExceeded` で失敗します。サーバーがペイロードの展開後の大きさを記録しているときは、展開結果がそれと一致しなければならず、一致しないとイベントは `kMalformedValue` で失敗します。

## 低レベルの関数

| 関数 | 処理 |
|---|---|
| `IsMariaDbCompressedEvent(type_code)` | 165-171 で true。 |
| `DecompressMariaDbEvent()` | 非圧縮のイベントを組み立て直します。非圧縮のヘッダと本体を持つ `DecompressedEvent` を返します。 |
| `ParseTransactionPayloadEvent()` | ペイロードのヘッダのフィールドを読みます。圧縮の種類、ペイロードの大きさ、展開後の大きさ、ペイロードへのポインタ。 |
| `DecompressTransactionPayload()` | ペイロードを、中のイベントを連結したものに展開します。 |
| `NextPayloadEvent()` | 中のイベントを 1 つ（`PayloadEventView`）読み、オフセットを進めます。オフセットが大きさより小さい間ループします。 |
