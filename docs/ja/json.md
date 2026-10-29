# JSON

MySQL は JSON 列を独自のバイナリ形式で格納し、行イベントはそのバイナリ文書を運びます。MariaDB は JSON を LONGTEXT として格納するので、MariaDB の JSON 列は文書のテキストを持つ `kText` として届き、このページの内容は要りません。

## バイナリ JSON からテキストへ

MySQL の JSON 列は `Value::Kind::kJson` にデコードされ、`Value::bytes` にバイナリ文書が入ります。`JsonBinaryToText()` は、サーバーの `CAST(j AS CHAR)` とバイト単位で同じテキストを出力します。

```cpp
std::string text;
const auto* data = reinterpret_cast<const uint8_t*>(value.bytes.data());
if (mybinlog::JsonBinaryToText(data, value.bytes.size(), &text) == mybinlog::Status::kOk) {
  // text is e.g. {"a": 1, "b": [true, null]}
}
```

サーバーのテキストには決まった規則があり、出力もそれに従います。区切りは `", "` と `": "`、オブジェクトのメンバは格納順（キーを長さ順、次にバイト順に並べた順）、double は往復で元に戻る最短の形で整数値には `.0` を付け、DATE、TIME、DATETIME、TIMESTAMP の値は引用符付きの文字列、DECIMAL は引用符なし、それ以外の不透明な値は `"base64:typeNN:..."` です。文字列は `"`、`\`、制御バイトをエスケープし、非 ASCII を含むほかのバイトはそのまま通します。

空の値（0 バイト）は JSON の null リテラルです。NOT NULL 列に NULL を押し込んだときにサーバーが格納する値です。`kJson` の値に対する `FormatValue()` は `JsonBinaryToText()` を呼びます。

## 文書の木

`ParseJsonBinary()` は文書を `JsonValue` の木にデコードします。文書全体を出力するのでなく、フィールドを読みたい呼び出し側のためのものです。

```cpp
mybinlog::JsonValue doc;
if (mybinlog::ParseJsonBinary(reinterpret_cast<const uint8_t*>(value.bytes.data()),
                              value.bytes.size(), &doc) == mybinlog::Status::kOk) {
  if (const mybinlog::JsonValue* name = mybinlog::FindJsonMember(doc, "name");
      name != nullptr && name->kind == mybinlog::JsonValue::Kind::kString) {
    // name->bytes holds the utf8mb4 string
  }
}
```

| `JsonValue::Kind` | データ |
|---|---|
| `kNull` | なし |
| `kBoolean` | `b` |
| `kInt`、`kUInt` | `i`、`u`（バイナリの int16/32/64 と uint16/32/64） |
| `kDouble` | `d` |
| `kString` | `bytes`（utf8mb4） |
| `kOpaque` | `opaque_type` と `bytes`。その他の MySQL の値です。DATE、TIME、DATETIME、TIMESTAMP は 8 バイトのパック済み temporal を、DECIMAL は精度、スケール、バイナリの 10 進数を持ちます。 |
| `kArray` | `elements` |
| `kObject` | `members`（格納順） |

`JsonValue` は自分のバイトをすべて所有し、コピーすると部分木ごとコピーされます。`JsonToText()` は木を `JsonBinaryToText()` と同じ形で出力します。`kJsonMaxDepth`（100、サーバーの上限）より深い入れ子は `kLimitExceeded` で失敗し、バイナリ文書内のオフセットと長さはすべて範囲を検査します。

## 部分更新

`binlog_row_value_options=PARTIAL_JSON` のとき、MySQL は `JSON_SET`、`JSON_REPLACE`、`JSON_REMOVE` で JSON 列を変える UPDATE を、新しい文書全体ではなく編集の列としてログに書けます。このような更新は PARTIAL_UPDATE_ROWS イベント（型 39）として届きます。その変更後イメージでは、編集を持つ JSON 列が `Value::Kind::kJsonDiff` にデコードされます。ほかの列と、サーバーが全体を書いた行は、UPDATE_ROWS と同じようにデコードされます。

差分を書くかどうかは行イメージと更新の内容で決まるので、読み手は同じ列で両方の種類を扱う必要があります。

```cpp
// previous: the column's last known kJson value; after: the after-image value.
mybinlog::Status NewJsonText(const mybinlog::Value& previous, const mybinlog::Value& after,
                             std::string* text) {
  const auto* before_bytes = reinterpret_cast<const uint8_t*>(previous.bytes.data());
  const auto* after_bytes = reinterpret_cast<const uint8_t*>(after.bytes.data());
  if (after.kind == mybinlog::Value::Kind::kJson) {
    return mybinlog::JsonBinaryToText(after_bytes, after.bytes.size(), text);
  }
  return mybinlog::ApplyJsonDiffsToBinary(before_bytes, previous.bytes.size(), after_bytes,
                                          after.bytes.size(), text);
}
```

差分は列の直前の値に適用します。FULL の行イメージではその値が変更前イメージにあります。MINIMAL では変更前イメージが JSON 列を持たないので、読み手がその行について最後に見た値を渡す必要があります。

各段階は個別にも使えます。

| 関数 | 処理 |
|---|---|
| `ParseJsonDiffs()` | `kJsonDiff` の値を、ログの順に `JsonDiff` のレコード（操作、パス、値）にデコードします。 |
| `ApplyJsonDiffs()` | レコードをサーバーと同じ意味で `JsonValue` に適用し、サーバーが格納した文書を得ます。差分が挿入したオブジェクトのメンバはサーバーが格納する位置に入るので、`JsonToText()` はサーバーと同じテキストを出力します。 |
| `ApplyJsonDiffsToBinary()` | 直前の値に `ParseJsonBinary()` を適用し、上の 2 つを行い、`JsonToText()` で出力します。 |

`JsonDiff::path` はサーバーが JSON パスを出力する形（`$.a`、`$."b c"`、`$[0]`、`$[last]`）です。`ApplyJsonDiffs()` はワイルドカードと範囲を含まない任意の MySQL のパスを受け付けます。`kJsonDiff` の値に対する `FormatValue()` は、編集を `{"op": "REPLACE", "path": "$.a", "value": ...}` のオブジェクトの JSON 配列として出力し、REMOVE には `value` のメンバを付けません。
