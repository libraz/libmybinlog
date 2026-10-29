# エラーと検証

libmybinlog の関数は例外を投げません。失敗しうる関数はすべて `mybinlog::Status`（`mybinlog/status.h`）を返し、結果を末尾の出力ポインタで渡します。失敗したときは、ドキュメントに別の記載がない限り出力に触れません。`Status` を返す関数には `[[nodiscard]]` が付いています。

## ステータスの値

| `Status` | 意味 |
|---|---|
| `kOk` | 成功。 |
| `kTruncated` | 構造が完結する前に入力が終わりました。 |
| `kInvalidMetadata` | 列やテーブルのメタデータが矛盾しているか範囲外です。TABLE_MAP と一致しない列数、サーバーが書かない長さプレフィックスの幅、TABLE_MAP が先に来ていない行イベントなど。 |
| `kMalformedValue` | 値のバイト列がその型の正しいエンコードではありません。月が 13 の DATE、壊れた圧縮ストリーム、ヘッダより短い `event_length` など。 |
| `kUnsupported` | 形式は正しいがライブラリがデコードしない入力です。未知の列型や圧縮方式、zstd なしのビルドでの zstd ペイロードなど。 |
| `kChecksumMismatch` | CRC32 のトレーラが一致しないか、チェックサム付きの枠がトレーラを持つには短すぎます。 |
| `kLimitExceeded` | 設定した大きさや予算の上限を超えました。 |
| `kNeedMoreData` | `Decoder::Next()` だけが返します。完全なイベントがまだバッファにありません。エラーではありません。 |

`StatusName()` はログ用に列挙子の名前（`"kTruncated"`）を返します。各関数の Doxygen コメントに、返すステータスとその条件が書かれています。

## デコーダのエラー

デコーダは位置と文脈を加えます。`Decoder::last_error()` は、ステータス、失敗したイベントのストリーム上のオフセット、イベントの型と理由を示すメッセージを返します。エラーは `Reset()` まで残り続けます。[Decoder](decoder.md) を参照してください。

## 検証

ライブラリは入力を信頼できないものとして扱います。どの長さも使う前に残りのバイト数と比べ、サーバーが書かないメタデータや値は推測せずに拒否します。幅の分からない TABLE_MAP の列型はイベントごと失敗させます。推測を誤ると以降の全列がずれるためです。たとえば、範囲外の temporal のフィールド、1..65 の範囲外の DECIMAL の精度、1..2 バイトの範囲外の ENUM の大きさ、64 ビットより広い BIT、コンテナの外を指す JSON のオフセットを拒否します。

メモリの使用量は明示的な上限で抑え、超えるとどれも `kLimitExceeded` で失敗します。

| 上限 | 既定値 | 制限するもの |
|---|---|---|
| `DecoderOptions::max_event_size` | 1 GiB | イベント 1 つの長さ。 |
| `DecoderOptions::max_decompressed_size` | 1 GiB | 展開した MariaDB イベントまたはトランザクションペイロード 1 つ。 |
| `DecodeOptions::max_value_bytes` | なし | デコードした値 1 つ。 |
| `DecodeOptions::max_decompressed_bytes` | 64 MiB | MariaDB の COMPRESSED 列の値 1 つ。 |
| `DecodeBudget` | イベントごと、本体から決まる大きさ | 行イベント 1 つが実体化するものすべて。[行](rows.md)を参照してください。 |
| `kMaxTableColumns` | 4096 | TABLE_MAP の列数。 |
| `kJsonMaxDepth` | 100 | JSON の入れ子。 |
| `GtidSet::kMaxIntervalsPerTsid` | 65536 | GTID のソースごとの区間数。 |

## スレッド安全性

free 関数は再入可能です。`Decoder`、`ChecksumTracker`、`DecodeBudget` は 1 つのストリームまたは 1 回のデコードのための可変の状態で、2 つのスレッドから同時に使ってはいけません。`GtidSet` と `MariaDbGtidPosition` は同時に読めますが、同時に変更はできません。`kServerSettingRules` は不変です。
