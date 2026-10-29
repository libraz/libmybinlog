# Decoder

`mybinlog::Decoder`（`mybinlog/decoder.h`）は、バイトストリームを型付きのイベントに変換します。pull 型のデコーダで、呼び出し側が `Feed()` でバイトを渡し、`Next()` でイベントを取り出します。読み込み、バッファリング、停止の制御は呼び出し側のループに残ります。

## バイトの投入とイベントの取り出し

```cpp
mybinlog::Decoder decoder;
decoder.Feed(data, size);  // a whole file, or any split of a stream
mybinlog::Event event;
mybinlog::Status status;
while ((status = decoder.Next(&event)) == mybinlog::Status::kOk) {
  // use event
}
// status is kNeedMoreData, or an error
```

`Feed()` はバイトをコピーするので、呼び出し側のバッファは戻った直後から再利用できます。分割の仕方は結果に影響しません。ファイル全体を 1 回で渡しても、1 バイトずつ渡しても、同じイベント列になります。

`Next()` は次の 3 通りのどれかを返します。

| 結果 | 意味 |
|---|---|
| `kOk` | `*out` に次のイベントが入っています。 |
| `kNeedMoreData` | バッファ内のバイトに完全なイベントがありません。続きを投入するか、入力が終わったなら `Finish()` を呼びます。エラーではなく、`*out` には触れません。 |
| それ以外 | ストリームのデコードに失敗しました。`last_error()` が場所と理由を示します。 |

`buffered_bytes()` は、投入されたうち `Next()` がまだ消費していないバイト数を返します。

## 入力の終わり

これ以上バイトが来ないときは、`Next()` が `kNeedMoreData` を返した後に `Finish()` を呼びます。ストリームがイベントの境界で終わっていれば `kOk`、バッファにイベント（または 4 バイトのファイルマジック）の一部が残っていれば `kTruncated` を返します。サーバーが書き込み中のファイルはイベントの途中で終わることがあるため、稼働中のファイルを追いかける読み手は `Finish()` を呼ばずに投入を続けます。

## エラーは残り続ける

`Next()` がエラーを返すと、以降の `Next()` と `Finish()` は `Reset()` まで同じステータスを返します。次のイベントの境界は失敗したイベントの長さフィールドからしか分からないので、枠の切り出しに失敗したストリームを安全に再同期する方法がないためです。

`last_error()` は `DecodeError` を返します。

| メンバ | 内容 |
|---|---|
| `status` | 失敗したステータス。構築か `Reset()` 以降に失敗がなければ `kOk`。 |
| `offset` | 失敗したイベントのストリーム上のオフセット。TRANSACTION_PAYLOAD 内のイベントでは、ペイロードイベントのオフセット。 |
| `message` | ログ用の、イベント型、オフセット、理由。例: `TABLE_MAP_EVENT at offset 1180: body does not parse (kTruncated)`。 |

`Reset()` はバッファ内のバイトと状態をすべて捨て、同じオプションで構築した直後の状態に戻します。各ステータスの意味は[エラーと検証](errors-and-validation.md)にあります。

## Event の中身

| メンバ | 内容 |
|---|---|
| `header` | 19 バイトの共通ヘッダ。展開した MariaDB の圧縮イベントでは、対応する非圧縮イベントのヘッダ。 |
| `body` | 解析した本体の `std::variant`。`std::get_if<T>(&event.body)` で読みます。 |
| `disposition` | ストリームの FDE が示した種類のもとでの、ヘッダの `ClassifyEvent()`。[イベントの扱い](disposition.md)を参照してください。 |
| `from_payload` | TRANSACTION_PAYLOAD から取り出したイベントで true。 |
| `offset` | イベントの先頭バイトのストリーム上のオフセットで、ファイルでは binlog の位置です。TRANSACTION_PAYLOAD から取り出したイベントでは、ペイロードイベントのオフセット。 |

本体の選択肢はヘッダの型コードで決まります。

| 型コード | 選択肢 |
|---|---|
| FORMAT_DESCRIPTION (15) | `FormatDescriptionEvent` |
| ROTATE (4) | `RotateEvent` |
| TABLE_MAP (19) | `TableMap` |
| WRITE/UPDATE/DELETE_ROWS v1 (23-25) と v2 (30-32)、PARTIAL_UPDATE_ROWS (39)、展開後の MariaDB 166-171 | `TableRowsEvent` |
| QUERY (2)、展開後の MariaDB 165 | `QueryEvent` |
| XID (16) | `XidEvent` |
| GTID (33)、GTID_TAGGED (42) | `GtidEvent` |
| PREVIOUS_GTIDS (35) | `PreviousGtidsEvent` |
| MariaDB GTID (162) | `MariaDbGtidEvent` |
| MariaDB GTID_LIST (163) | `MariaDbGtidListEvent` |
| MariaDB ANNOTATE_ROWS (160) | `MariaDbAnnotateRowsEvent` |
| HEARTBEAT (27)、HEARTBEAT_V2 (41) | `HeartbeatEvent` |
| その他 | `UnknownEvent` |

イベントはすべての型について返ります。文の実行コンテキスト（INTVAR、RAND、USER_VAR）、ROWS_QUERY、ライブラリにパーサのない型も、生の本体を持つ `UnknownEvent` として届くので、黙って読み飛ばされるイベントはありません。扱いが `kFailClosed` のイベントでもデコーダは止まりません。イベントを返し、判断は呼び出し側に任せます。

展開が有効なとき、TRANSACTION_PAYLOAD 自体は返りません。中のイベントが順に 1 回の `Next()` につき 1 つずつ、それぞれ `from_payload` を立てて返ります。

### 行イベント

`TableRowsEvent` は行イベントと、それが参照するテーブルを持ちます。

| メンバ | 内容 |
|---|---|
| `table_id`、`flags` | ポストヘッダの値。`flags & kRowsStatementEndFlag` は文の最後の行イベントを示します。 |
| `table` | テーブル ID が参照する `TableMap`。デコーダが保持しています。 |
| `decoded` | `rows` にデコードしたイメージが入っているかどうか。`DecoderOptions::decode_rows` が無効なときや、イベントにテーブルがないときは false。 |
| `rows` | 行イメージ。[行](rows.md)を参照してください。 |
| `body`、`size` | ヘッダの後ろの生の本体。トレーラは含みません。 |

テーブル ID に対応する TABLE_MAP が先に来ていない行イベントは `kInvalidMetadata` で失敗します。例外は `kDummyTableId`（48 ビットすべてが 1）で、MySQL がテーブルを指さない行イベントに書く値です。このイベントは `table == nullptr`、`decoded == false` で届きます。

デコーダは FORMAT_DESCRIPTION と ROTATE のたびにテーブルの対応を捨て、同じ ID の TABLE_MAP が来ると前のものを置き換えます。サーバーは文ごとに行イベントの前に TABLE_MAP を書くので、対応がファイルをまたいで必要になることはありません。

## Event 内のポインタの寿命

デコーダが所有するメモリを指すメンバが 3 つあります。`UnknownEvent::body`、`TableRowsEvent::table`、`TableRowsEvent::body` です。これらはデコーダの次の `Next()`、`Feed()`、`Reset()` まで有効です。`Event` のそれ以外（文字列、vector、デコードした `Value`）はイベントが所有しているので、ムーブも保持もできます。

テーブルの記述をそれより長く使うには、`TableMap` をコピーします。

```cpp
if (const auto* rows = std::get_if<mybinlog::TableRowsEvent>(&event.body); rows && rows->table) {
  mybinlog::TableMap kept = *rows->table;  // owned copy
}
```

## オプション

`DecoderOptions` はコンストラクタに渡し、コピーされます。既定値はどれも、信頼できるサーバーの binlog ファイルに合わせてあります。

| オプション | 既定値 | 効果 |
|---|---|---|
| `default_checksum` | `kCrc32` | 最初の FDE より前に仮定するチェックサム方式。意味を持つのはレプリケーションストリームだけで、その人工の ROTATE は FDE より先に来ます。レプリカがソースに伝えた方式を設定します。 |
| `verify_checksum` | `true` | CRC32 のトレーラを照合します。`false` でも方式に従って枠は切り出しますが、照合はしません。 |
| `max_event_size` | 1 GiB | イベントの `event_length` の上限。超えると `kLimitExceeded` で失敗します。 |
| `decode_rows` | `true` | 行イベントを `TableRowsEvent::rows` にデコードします。`false` では生の本体だけが残ります。 |
| `rows` | `DecodeOptions()` | 行のデコードでの列の選択とメモリの上限。[行](rows.md)を参照してください。 |
| `expand_compressed` | `true` | MariaDB の圧縮イベントと TRANSACTION_PAYLOAD を展開します。`false` では、どちらも扱いが `kCompressed` の `UnknownEvent` として届きます。 |
| `max_decompressed_size` | 1 GiB | 展開した MariaDB イベントの本体 1 つ、またはトランザクションペイロード 1 つの上限。 |

`rows.budget` が null のときは、行イベントごとに本体の大きさから決めた専用の予算で計上します。null でない予算はそのまま使われ、イベントをまたいで累積します。この予算はデコーダより長く生存している必要があります。

## レプリケーションストリームの読み込み

デコーダは 4 バイトのマジックのないストリームも読みます。レプリケーションのダンプで、クライアントが各パケットのヘッダと先頭のステータスバイトを取り除いた後の形です。各イベントのバイトを届いた順に投入します。ファイルとの違いは 3 つあります。

- ストリームは、どの FDE よりも前に人工の ROTATE で始まります。`default_checksum` には、レプリカが（接続で `@source_binlog_checksum` や `@master_binlog_checksum` を設定して）伝えた方式を設定します。
- 動きのないストリームには HEARTBEAT と HEARTBEAT_V2 が現れます。扱いが `kDataNeutral` の `HeartbeatEvent` として届きます。
- `offset` はこのデコーダに投入したバイト数で、ソース上の位置ではありません。ソースの座標にはヘッダの `next_position` か、ROTATE の `position` を使います。

デコーダはファイルとストリームを先頭 4 バイトで見分けます。マジックのないストリームで最初のイベントのタイムスタンプが 1852400382（2028-09-11）だと、マジックと同じバイト列になり、ファイルと誤認されます。

## スレッド安全性

1 つのデコーダは 1 つのストリームに使い、同時には 1 つのスレッドからだけ使います。別々のデコーダは互いに独立していて、別々のスレッドで動かせます。
