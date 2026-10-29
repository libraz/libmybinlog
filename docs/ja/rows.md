# 行

行イベントは 1 つのテーブルの行イメージを運びます。デコーダはそれを `RowsEvent`（`mybinlog/row_decoder.h`）にデコードし、低レベルの `DecodeRowsEvent()` は呼び出し側が切り出した本体に同じことをします。

## イベント内の行イメージ

| イベント | イメージの入る場所 |
|---|---|
| WRITE_ROWS (23, 30) | `rows`: 挿入した行ごとに 1 つ。 |
| DELETE_ROWS (25, 32) | `rows`: 削除した行ごとに 1 つ。 |
| UPDATE_ROWS (24, 31) | `updates`: 行ごとに `(before, after)` の組。 |
| PARTIAL_UPDATE_ROWS (39) | UPDATE_ROWS と同じく `updates`。変更後イメージの JSON 列が部分更新を持つことがあります。[JSON](json.md)を参照してください。 |

イベントがどの種類の変更かはヘッダの `type_code` で分かります。MariaDB の圧縮された行イベントは、展開後に非圧縮の型コードで届きます。

`Row` は TABLE_MAP の列ごとに 1 つずつ、列の順に並んだ `std::vector<Value>` で、`row[i]` は `table.columns[i]` に対応します。大きな文は複数の行イベントに分かれ、`TableRowsEvent::flags` の `kRowsStatementEndFlag` が最後のものを示します。

MySQL のパーティション化されたテーブルの変更では `has_partition_id` が立ち、`partition_id` は行のあるパーティション（UPDATE では更新後）を示します。UPDATE と PARTIAL_UPDATE では、`source_partition_id` が更新前に行のあったパーティションです。

## NULL、イメージにない列、選択しなかった列

値なしで返る状況は 3 つあり、取り違えてはいけません。

| `Value::kind` | 意味 |
|---|---|
| `kNull` | 行のこの列は SQL の NULL です。 |
| `kAbsent` | イメージがこの列を持っていません。`binlog_row_image=MINIMAL` や `NOBLOB` ではサーバーが列を省きます。値は不明であり、NULL ではありません。 |
| `kNull`（選択外） | `DecodeOptions::columns` がこの列を選んでいません。行が NULL を持っていても、イメージが列を省いていても `kNull` になります。 |

イメージに何が入るかは行イメージの設定で決まります。

| `binlog_row_image` | イメージ |
|---|---|
| FULL | 両方のイメージに全列。 |
| MINIMAL | 変更前イメージには行を特定する列（主キー）、変更後イメージには文が変えた列だけ。書き込みのイメージは、文が設定しなかった列を省くことがあります。 |
| NOBLOB | FULL と同じですが、キーでも変更対象でもない BLOB、TEXT、JSON の列を省きます。 |

MINIMAL や NOBLOB のイメージから変更を再生するには、変更前イメージのキー列で行を特定し、手元の行に変更後イメージを重ねます。`kAbsent` の列は現在の値を保ちます。MINIMAL の更新の変更前イメージは変更した列の古い値を持たないので、それが必要な読み手はテーブルの状態を自分で保持する必要があります。

## TABLE_MAP に必要な情報

行は直前の TABLE_MAP に従ってデコードします。デコーダが必要とする情報の一部は TABLE_MAP の任意メタデータにあり、サーバーは `binlog_row_metadata` に従ってそれを書きます。

| TABLE_MAP のフィールド | 書かれる設定 | ないときの結果 |
|---|---|---|
| SIGNEDNESS（`TableMap::has_signedness`） | MINIMAL と FULL | 整数は符号付きとしてデコードされ、`INT64_MAX` を超える BIGINT UNSIGNED は負の数になります。 |
| 文字セット（`TableMapColumn::has_charset`、`charset`） | MINIMAL と FULL | 文字データは `kText` ではなく `kBytes` になります。 |
| 列名（`has_column_names`、`name`） | FULL | `TableMapColumn::name` が空になります。 |
| ENUM と SET のラベル（`has_enum_labels`、`has_set_labels`、`labels`） | FULL | `FormatEnumLabel()` と `FormatSetLabels()` が失敗します。序数とビットマスクはデコードできます。 |

MariaDB の `binlog_row_metadata=NO_LOG` はこのどれも書きません。列の型と型のメタデータは常にあるので、設定にかかわらずすべての値をデコードし、長さを測れます。

## 列の選択とメモリの上限

`DecodeOptions` は、デコードで何を実体化するかを決めます。`DecoderOptions::rows` として渡すか、`DecodeRowsEvent()` に直接渡します。

| メンバ | 既定値 | 効果 |
|---|---|---|
| `columns` | 空 | デコードする列を TABLE_MAP の序数で指定します。空なら全列です。`false` の列と範囲外の列は長さを測って読み飛ばします。メタデータは検証しますが、値は実体化も検査もしません。 |
| `max_value_bytes` | 0（なし） | 1 つの値のペイロードの上限。超えるとイベントが `kLimitExceeded` で失敗します。 |
| `max_decompressed_bytes` | 64 MiB | MariaDB の COMPRESSED 列の値 1 つを展開した大きさの上限。0 で上限なし。 |
| `budget` | null | イベントが実体化するものすべてを計上する `DecodeBudget`。 |
| `per_value_overhead` | 0 | 各行イメージの `Value` 1 つあたりに計上するバイト数。 |
| `per_row_overhead` | 0 | デコードした行 1 つ（UPDATE では組 1 つ）あたりに計上するバイト数。 |

ワイヤ上の大きさは、行の数も圧縮された値の展開後の大きさも制限しません。NULL はワイヤ上では 1 ビットですが、メモリ上では `Value` 1 つ分を占めます。`DecodeBudget` は、大きさに比べてはるかに多くを確保させるイベントへの防御です。行イメージは埋める前に `列数 * per_value_overhead` を、値はコピーする前にペイロードのバイト数を、行はデコード後に `per_row_overhead` を計上します。予算が尽きるとデコードは `kLimitExceeded` で失敗します。`budget.used` でイベントにかかった量を読めます。

`DecodeBudget::ForEventBody(body_bytes, expansion_ceiling)` は、少なくとも `expansion_ceiling` で、本体の大きさに応じて増える予算を作ります。`DecoderOptions::rows.budget` が null のとき、デコーダは `rows.max_decompressed_bytes` を下限としてこれを行イベントごとに使います。呼び出し側のアロケータに即してイベントごとのメモリを制限するには、オーバーヘッドを設定します。

```cpp
mybinlog::DecoderOptions options;
options.rows.per_value_overhead = sizeof(mybinlog::Value);
options.rows.per_row_overhead = sizeof(mybinlog::Row);
mybinlog::Decoder decoder(options);
```

予算は渡されたデコードによって書き換わるので、並行するデコードで共有してはいけません。`DecoderOptions::rows.budget` で渡した予算は、そのデコーダの全行イベントにわたって累積します。

## 行を集めずに受け取る

`VisitRowsEvent()` は `DecodeRowsEvent()` と同じ引数を取り、`RowsEvent` のかわりに `RowScratch`、`RowVisitorFn`、コンテキストのポインタを受け取ります。すべての行は `RowScratch` にデコードされ、呼び出しをまたいで保持すれば、続く行イベントは最初のあと `Row` を割り当てません。ビジターは各行イメージを受け取り、UPDATE と PARTIAL_UPDATE では 2 番目の引数で更新後のイメージを受け取ります（それ以外では null）。どちらのイメージも作業領域に属し、デコーダが次の行で上書きします。ビジターはそこからペイロード（`Value::bytes`）をムーブしてよいですが、ポインタを保持してはいけません。

値、予算の課金、ステータスは `DecodeRowsEvent()` と同じです。行は、その行と予算の課金が成功してから渡されます。ビジターが `kOk` 以外を返すとデコードは止まり、そのステータスがそのまま返ります。失敗より前の行はすでに渡されているので、イベントを丸ごと公開する必要がある呼び出し側は、`VisitRowsEvent()` が `kOk` を返すまで受け取った行を保持します。v2 の追加行情報のパーティション ID は `DecodeRowsEvent()` だけが報告します。

## ワーカースレッドでのデコード

`DecodeRowsEvent()` は再入可能で、`TableMap` を呼び出しの間だけ読みます。読み手は 1 つのスレッドでイベントを切り出して解析し、行イベントを `TableMap` のコピーと一緒にワーカースレッドへ渡せます。[低レベル API](low-level-api.md)を参照してください。
