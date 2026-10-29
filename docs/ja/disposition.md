# イベントの扱い

データを失ってはならない読み手は、イベントの型ごとに、デコードするか、読み飛ばすか、止まるかを決める必要があります。`ClassifyEvent()`（`mybinlog/disposition.h`）はその判断を 1 か所で行い、デコーダは結果を `Event::disposition` に入れます。

## 扱いの種類

| `Disposition` | 意味 |
|---|---|
| `kFraming` | ストリームの枠で、読み手が消費します。FORMAT_DESCRIPTION、ROTATE、STOP。 |
| `kTableMap` | TABLE_MAP。テーブルの記述にデコードします。 |
| `kRows` | 非圧縮の行イベント（PARTIAL_UPDATE_ROWS を含む）。デコードして適用します。 |
| `kGtid` | 位置イベント。MySQL の GTID か GTID_TAGGED、MariaDB の GTID か GTID_LIST。 |
| `kTransactionBoundary` | XID。開いているトランザクションを確定します。 |
| `kQuery` | QUERY。BEGIN、COMMIT、DDL、文のテキスト。 |
| `kCompressed` | MariaDB の圧縮イベントか TRANSACTION_PAYLOAD。[圧縮イベント](compression.md)を参照してください。 |
| `kDataNeutral` | 情報のみ。読み飛ばしても行も位置も失いません。 |
| `kFailClosed` | 適用できず、読み飛ばしても無害だと証明もできません。止まります。 |

`EventDisposition::reason` はログ用に判断の理由を説明する静的な文字列で、`DispositionName()` は列挙子の名前を返します。

デコーダは扱いを記録するだけです。`kFailClosed` を含むすべてのイベントを返し、止まるかどうかは呼び出し側に任せます。

## 型の分類

| 扱い | イベントの型 |
|---|---|
| `kDataNeutral` | HEARTBEAT と HEARTBEAT_V2。INTVAR、RAND、USER_VAR（続く文のコンテキストで、その QUERY で判断します）。BEGIN_LOAD_QUERY、APPEND_BLOCK、DELETE_FILE。IGNORABLE。ROWS_QUERY。PREVIOUS_GTIDS。TRANSACTION_CONTEXT と VIEW_CHANGE（グループレプリケーション）。MariaDB では ANNOTATE_ROWS、BINLOG_CHECKPOINT、START_ENCRYPTION |
| `kFailClosed` | ANONYMOUS_GTID（記録する位置がない）。XA_PREPARE（確定しないかもしれない準備済みの行）。INCIDENT（ソースがイベントの欠落を宣言した）。EXECUTE_LOAD_QUERY（ステートメントベースの LOAD DATA で、行が実体化されない）。START_EVENT_V3 など、対応するサーバーが書かない型。MariaDB の PARTIAL_ROW_DATA |

ほかの既知の型は、最初の表の該当する行の扱いになります。

サーバーの種類が影響します。MySQL のソースで MariaDB の番号の型（160 以上）が来たとき、また MariaDB のソースで MySQL の GTID の型（33、42）が来たときは、そのサーバーが書かない型なので止まる扱いです。デコーダは最新の FDE が示した種類で分類します。

`EventType` の範囲外のコードは、ヘッダのフラグで分類します。ソースが `LOG_EVENT_IGNORABLE_F`（`kLogEventIgnorableFlag`）を立てていれば `kDataNeutral`、そうでなければ `kFailClosed` です。このフラグは、古い読み手が読み飛ばしてよい新しいイベント型にサーバーが付ける印です。

この扱いは、行ベースの変更を読む読み手のポリシーを表しています。たとえば ANONYMOUS_GTID が止まる扱いなのは、そのような読み手が GTID の位置を記録するからです。binlog ビューアのように別の目的を持つツールは、扱いを無視して `header.type_code` を見てかまいません。

## 読み手での使い方

```cpp
switch (event.disposition.disposition) {
  case mybinlog::Disposition::kFailClosed:
    // stop, and report event.disposition.reason with event.offset
    break;
  case mybinlog::Disposition::kDataNeutral:
  case mybinlog::Disposition::kFraming:
    break;  // nothing to apply
  default:
    // handle the parsed body
    break;
}
```

`ClassifyEvent()` には 2 つのオーバーロードがあります。既知の `EventType` を受け取るものと、生の `type_code` とヘッダの `flags` を受け取り、列挙の範囲外のコードも扱うものです。どちらも再入可能です。
