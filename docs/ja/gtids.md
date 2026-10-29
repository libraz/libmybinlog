# GTID

MySQL と MariaDB は、どちらもグローバルトランザクション ID でトランザクションに名前を付けますが、形式に互換性はありません。ライブラリは両者を別の型として扱います。`mybinlog/mysql_gtid.h` と `mybinlog/mariadb_gtid.h` です。

| | MySQL | MariaDB |
|---|---|---|
| GTID 1 つ | `uuid:gno` または `uuid:tag:gno` | `domain-server-seq` |
| 各トランザクションの前のイベント | GTID (33) または GTID_TAGGED (42) → `GtidEvent` | GTID (162) → `MariaDbGtidEvent` |
| 各ファイルの先頭のイベント | PREVIOUS_GTIDS (35) → `PreviousGtidsEvent` | GTID_LIST (163) → `MariaDbGtidListEvent` |
| 実行済みトランザクションの集合 | `GtidSet`: ソースごとの区間 | `MariaDbGtidPosition`: ドメインごとの最後の GTID |

## MySQL の GTID

MySQL の GTID は、ソースサーバーの UUID、任意のタグ、トランザクション番号（GNO、`1 <= gno < 2^63 - 1`）からなります。UUID とタグの組がトランザクションのソース `GtidSet::Tsid` です。タグはサーバーの規則 `[a-z_][a-z0-9_]{0,31}` に従い、小文字にして保持します。`NormalizeGtidTag()` がタグを検証して小文字にします。

`GtidEvent` は、続くトランザクションの GTID を持ちます。`ToString()` は `uuid:gno`、タグ付きの GTID では `uuid:tag:gno` を出力します。`gtid_next='AUTOMATIC:<tag>'` で実行したトランザクションは GTID_TAGGED イベントを生み、`tag` の入った同じ `GtidEvent` として届きます。

`ParseUuid()` と `FormatUuid()` は、16 バイトの `Uuid` と 8-4-4-4-12 の 16 進テキストを相互に変換します。

### GtidSet

`GtidSet` は MySQL の GTID の集合で、TSID ごとにソート・統合した区間として保持します。サーバーのバイナリ形式と同じ形です。再開位置やチェックポイントに使う型です。

```cpp
mybinlog::GtidSet executed;
if (mybinlog::GtidSet::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:mytag:1-2", &executed) ==
    mybinlog::Status::kOk) {
  std::string text = executed.ToString();
  // "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5,3e11fa47-71ca-11e1-9e33-c80aa9429562:mytag:1-2"
  std::vector<uint8_t> encoded;
  executed.EncodeBinary(&encoded);  // the GTID set COM_BINLOG_DUMP_GTID carries
}
```

| メンバ | 処理 |
|---|---|
| `Parse(text, out)` | サーバーが出力する形の集合のテキスト（`uuid:1-3:7:tag:1-4,uuid2:5`）を読みます。空集合は `""` だけです。`uuid:5` は GTID 5 だけを表し、再開の慣習として `1-5` に広げるかどうかは呼び出し側が決めます。 |
| `ToString()` | 正規のテキスト。TSID ごとに 1 項目で、範囲は常に `a-b` の形です。GTID 1 つは `5-5` と出力し、数だけの形にはなりません。 |
| `EncodeBinary(out)` | `COM_BINLOG_DUMP_GTID` 用のバイナリ形式。タグを持つ TSID がなければ形式 0、あれば形式 1 です。 |
| `DecodeBinary(data, size, out)` | PREVIOUS_GTIDS の本体などのバイナリ形式を読みます。 |
| `Add(uuid, tag, interval)`、`Merge(other)` | TSID に区間 `[start, end)` を 1 つ加えるか、別の集合との和をとります。 |
| `IsSubsetOf(other)`、`empty()`、`Clear()` | 集合の問い合わせと初期化。 |

`Interval` の終端は含みません。`{1, 6}` は `1-5` です。1 つの TSID が持てる区間は `kMaxIntervalsPerTsid`（65536）までで、超えると `Parse`、`Add`、`Merge`、`DecodeBinary` は `kLimitExceeded` で失敗します。信頼できない入力に対するメモリの上限です。

`GtidSet` は複数のスレッドから同時に読めますが、同時に変更はできません。

## MariaDB の GTID

MariaDB の GTID は、レプリケーションのドメイン、発生元のサーバー ID、シーケンス番号からなります。シーケンス番号は符号なし 64 ビットで、0 も有効な値です。`MariaDbGtid::Parse()` は `domain-server-seq` を読み、`IsMariaDbGtidFormat()` は結果を保持せずにテキストを判定します。

`MariaDbGtidEvent` は、続くトランザクションの GTID を持ちます。`server_id` はイベントのヘッダから取ります。`standalone()` は FL_STANDALONE、つまり DDL のように COMMIT や XID なしで終わるグループであることを示します。`flags` は flags2 の残りを持ちます。

位置は GTID の並びで、意味を持つ項目はドメインごとに 1 つ（シーケンス番号が最大のもの）です。区間の集合ではないので、別の型 `MariaDbGtidPosition` にしています。

```cpp
mybinlog::MariaDbGtidPosition required, available;
if (mybinlog::MariaDbGtidPosition::Parse("0-1-42,1-2-100", &required) == mybinlog::Status::kOk &&
    mybinlog::MariaDbGtidPosition::Parse("0-3-50,1-2-100", &available) == mybinlog::Status::kOk) {
  bool reached = mybinlog::PositionCovers(required, available);  // true
}
```

`PositionCovers()` はドメインごとに最大のシーケンス番号を比べ、サーバー ID は無視します。サーバー ID はフェイルオーバーで正当に変わるためです。位置は項目をテキスト（またはワイヤ上）の順に保ち、`ToString()` もその順で出力します。

`MariaDbGtidListEvent::gtids` はファイル先頭での binlog の状態、`MariaDbAnnotateRowsEvent::query` は続く行イベントを生んだ文で、後者は `binlog_annotate_row_events=ON` のときに書かれます。
