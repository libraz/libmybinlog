# 値

デコードした列の値は `mybinlog::Value`（`mybinlog/value.h`）です。ワイヤ上の内容を、構造化したまま描画せずに保持します。テキストへの変換は別の、任意の段階です。

## Value 構造体

`Value::kind` が、どのメンバに意味があるかを示します。それ以外のメンバは既定値のままです。

| メンバ | 使う種類 |
|---|---|
| `kind` | 常に使います。値の形。 |
| `type` | 常に使います。列の宣言上の型（TABLE_MAP の列の `RealType()`）で、ENUM と SET は `ColumnType::kEnum` と `kSet` になります。 |
| `i` | `kInt` |
| `u` | `kUInt`、`kYear`、`kEnum`、`kSet`、`kBit` |
| `d` | `kDouble` |
| `t` | `kDate`、`kDateTime`、`kTime` |
| `epoch_seconds`、`microsecond`、`fsp` | `kTimestamp` |
| `bytes` | `kDecimal`、`kText`、`kBytes`、`kJson`、`kJsonDiff` |

`kNull` と `kAbsent` はデータを持ちません。両者の違いは[行](rows.md)を参照してください。`ValueKindName()` はログ用に列挙子の名前を返します。

## 列の型と値の種類

| SQL の型 | TABLE_MAP の `ColumnType` | `Value::Kind` |
|---|---|---|
| TINYINT、SMALLINT、MEDIUMINT、INT、BIGINT | `kTiny`、`kShort`、`kInt24`、`kLong`、`kLongLong` | `i` に `kInt`。SIGNEDNESS が列を符号なしとしていれば `u` に `kUInt` |
| FLOAT、DOUBLE | `kFloat`、`kDouble` | `d` に `kDouble`（FLOAT は拡張） |
| DECIMAL | `kNewDecimal` | `kDecimal`: `bytes` に 10 進テキスト |
| BIT | `kBit` | `u` に `kBit` |
| YEAR | `kYear` | `u` に `kYear` |
| DATE | `kDate`、`kNewDate` | `t` に `kDate` |
| DATETIME | `kDatetime2`、`kDatetime` | `t` に `kDateTime` |
| TIME | `kTime2`、`kTime` | `t` に `kTime` |
| TIMESTAMP | `kTimestamp2`、`kTimestamp` | `epoch_seconds`、`microsecond`、`fsp` に `kTimestamp` |
| ENUM | `kString`（実際の型は `kEnum`） | `kEnum`: `u` に序数 |
| SET | `kString`（実際の型は `kSet`） | `kSet`: `u` にビットマスク |
| CHAR、BINARY | `kString` | `bytes` に `kText` または `kBytes` |
| VARCHAR、VARBINARY | `kVarchar` | `bytes` に `kText` または `kBytes` |
| TINYTEXT .. LONGTEXT、TINYBLOB .. LONGBLOB | `kBlob` | `bytes` に `kText` または `kBytes` |
| MariaDB の COMPRESSED な VARCHAR、TEXT、BLOB | `kVarcharCompressed`、`kBlobCompressed` | 展開済みの `kText` または `kBytes` |
| JSON（MySQL） | `kJson` | `kJson`: `bytes` にサーバーのバイナリ JSON |
| PARTIAL_UPDATE_ROWS の変更後イメージの JSON 列 | `kJson` | `kJsonDiff`: `bytes` に差分ベクタ |
| JSON（MariaDB。LONGTEXT の別名） | `kBlob` | `kText` |
| GEOMETRY とその派生型 | `kGeometry` | `kBytes`: 4 バイトの SRID に続く WKB |
| VECTOR | `kVector` | `kBytes` |
| 型付き配列（多値インデックス、レプリケーション専用） | `kTypedArray` | `kBytes` |
| 5.0 より前の DECIMAL | `kDecimal` | デコードしません: `kUnsupported` |

文字データは、TABLE_MAP が列にバイナリでない照合順序を与えていれば `kText`、バイナリの照合順序のときや TABLE_MAP に文字セットがないときは `kBytes` です。バイト列は格納された列の文字セットのままで、UTF-8 への変換はしません。変換する呼び出し側のために、`TableMapColumn::charset` が照合順序の ID を持っています。

MariaDB の INET4、INET6、UUID 型はバイナリ文字列としてログに書かれ、格納されたバイト列を持つ `kBytes` として返ります。

## 種類ごとの値の表し方

- **整数。** `kInt` は列の幅から符号拡張した値です。SIGNEDNESS のメタデータがないと、すべての整数を符号付きとしてデコードします。[行](rows.md)を参照してください。
- **浮動小数点数。** FLOAT は double に拡張するので、FLOAT の値を正確に比べるには `static_cast<float>(value.d)` で戻してから比べます。NaN と無限大はそのまま通ります。
- **DECIMAL。** `"-12.50"` のように、小数部がちょうど `scale` 桁の正確な 10 進テキストです。負のゼロ（`"-0.00"`）も書かれたまま保ちます。
- **DATE、DATETIME、TIME。** `Temporal` の暦と時刻のフィールドです。ゼロの日付や一部がゼロの日付（`0000-00-00`、`2024-05-00`）もフィールドのまま返し、NULL にはしません。`Temporal::negative` は TIME だけが使い、TIME の `hour` は 838 まであります。`fsp` は宣言された秒の小数部の精度、`microsecond` は小数部です。
- **TIMESTAMP。** UTC のエポック秒に、`microsecond` と `fsp` が加わります。タイムゾーンは適用しません。ゼロのタイムスタンプは `epoch_seconds == 0` のままです。
- **YEAR。** ゼロの年は 0、それ以外は 1901 から 2155 です。
- **ENUM。** ラベルの 1 始まりの序数です。0 は不正な挿入で格納される空の値です。
- **SET。** ビット n が宣言順で n 番目（0 から数える）のメンバを表すビットマスクです。
- **JSON。** 格納されたままのバイナリ文書です。`JsonBinaryToText()` でテキストにします。[JSON](json.md)を参照してください。

## 値をテキストにする

`mybinlog/format.h` は、サーバーのテキストプロトコルと同じ形で値を描画します。出力した値を `SELECT` の結果と比べられます。

| 種類 | `FormatValue()` の出力 |
|---|---|
| `kInt`、`kUInt` | 10 進の数字。 |
| `kDouble` | サーバーの `my_gcvt` の形。DOUBLE は往復で元に戻る最短の桁、FLOAT は有効数字 6 桁で、絶対値が大きいときと小さいときは指数表記（`1e15`、`1.5e-20`）。 |
| `kDecimal` | `bytes` そのまま。 |
| `kDate`、`kDateTime`、`kTime` | `YYYY-MM-DD`、`YYYY-MM-DD hh:mm:ss[.fraction]`、`[-]hh:mm:ss[.fraction]`。小数部は `fsp` 桁。ゼロの日付はゼロのまま出力。 |
| `kTimestamp` | DATETIME の形の UTC 日時。`time_zone='+00:00'` でのサーバーの出力と同じです。ゼロのタイムスタンプは `0000-00-00 00:00:00`。 |
| `kYear` | 4 桁。ゼロの年は `0000`。 |
| `kBit`、`kEnum`、`kSet` | `CAST(col AS UNSIGNED)` と同じ数値。 |
| `kText`、`kBytes` | `bytes` そのまま。エスケープも 16 進化もしません。 |
| `kJson` | サーバーの `CAST(j AS CHAR)` のテキスト。 |
| `kJsonDiff` | 差分ごとに `{"op", "path", "value"}` のオブジェクトを並べた JSON 配列。 |
| `kNull`、`kAbsent` | テキストはなく、`kInvalidMetadata` で失敗します。先に `kind` を調べ、独自の印を出力してください。 |

`FormatEnumLabel()` は ENUM の値のラベルを、`FormatSetLabels()` は SET の値のメンバをサーバーと同じく宣言順のカンマ区切りで返します。どちらも、TABLE_MAP が `binlog_row_metadata=FULL` のときだけ持つラベルを使い、ラベルがなければ `kInvalidMetadata` で失敗します。

```cpp
std::string text;
if (value.kind == mybinlog::Value::Kind::kEnum &&
    mybinlog::FormatEnumLabel(value, column, &text) == mybinlog::Status::kOk) {
  // text is the label, or "" for ordinal 0
}
```

テキスト化の関数は、ツール、テスト、サンプルのための便宜です。ライブラリの他の部分からは呼ばれません。
