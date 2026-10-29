# FAQ

## サーバーに接続して binlog をストリームで受け取れますか？

いいえ。ライブラリにネットワークのコードはありません。レプリケーションクライアントが接続し、レプリカとして登録し、`COM_BINLOG_DUMP` か `COM_BINLOG_DUMP_GTID` を送り、各パケットのヘッダとステータスバイトを取り除いて、イベントのバイト列を `Decoder` に投入します。ダンプ要求の GTID セットは `GtidSet::EncodeBinary()` で作れます。[Decoder](decoder.md) を参照してください。

## 列が kAbsent で返るのはなぜですか？

サーバーがその列を行イメージから省いたためです。`binlog_row_image=MINIMAL` や `NOBLOB` では、イメージは一部の列だけを持ちます。省かれた列の値は不明であり、NULL ではありません。[行](rows.md)を参照してください。

## 符号なし整数が負になるのはなぜですか？

TABLE_MAP に SIGNEDNESS のメタデータがないため、デコーダは符号なしの列を見分けられず、すべての整数を符号付きとしてデコードします。`binlog_row_metadata` を MINIMAL か FULL にしてください。フィールドの有無は `TableMap::has_signedness` で分かります。

## VARCHAR の列が kText でなく kBytes になるのはなぜですか？

TABLE_MAP に列の文字セットがない（行メタデータが無効）か、列がバイナリの照合順序を使っています。`binlog_row_metadata` が MINIMAL か FULL なら、文字列の列は `kText` になります。

## 列名が空なのはなぜですか？

列名は `binlog_row_metadata=FULL` のときだけ書かれます。ENUM と SET のラベルも同じです。

## 文字データは UTF-8 に変換されますか？

いいえ。`Value::bytes` は列の文字セットのバイト列のままで、変換する呼び出し側のために `TableMapColumn::charset` が照合順序の ID を持っています。

## TIMESTAMP をローカル時刻で得るには？

`Value::epoch_seconds` は UTC のエポック秒なので、必要なタイムゾーンを適用してください。`FormatValue()` は UTC の日時を出力します。

## Next() が同じエラーを返し続けるのはなぜですか？

エラーは残り続けます。失敗の後はストリームの位置が分からないので、デコーダは `Reset()` まで続行しません。[Decoder](decoder.md) を参照してください。

## MySQL の binlog が TRANSACTION_PAYLOAD で kUnsupported になるのはなぜですか？

サーバーが `binlog_transaction_compression=ON` で、このビルドに zstd がないためです。zstd をインストールし、`MYBINLOG_WITH_ZSTD=ON` でビルドしてください。どちらのビルドかは `HasZstdSupport()` で分かります。[圧縮イベント](compression.md)を参照してください。

## ステートメントベースの binlog をデコードできますか？

QUERY イベントとその文のテキスト、そして INTVAR、RAND、USER_VAR を生の `UnknownEvent` として返します。文を実行も解釈もしないので、行は得られません。

## 暗号化された binlog ファイルに対応していますか？

いいえ。MariaDB も MySQL も、レプリカに送る前に binlog のイベントを復号するので、レプリケーションストリームは読めますが、暗号化されたディスク上のファイルは読めません。

## デコードしないものは何ですか？

- 5.0 より前の DECIMAL の列（`kUnsupported`）。現在のサーバーは書きません。
- MariaDB の PARTIAL_ROW_DATA イベント。扱いが `kFailClosed` の `UnknownEvent` として届きます。
- v2 の行イベントにある NDB の追加行情報。読み飛ばします。
- MariaDB の INET4、INET6、UUID の値のテキスト化。格納されたバイト列のまま返ります。

## どのサーバーの版に対応していますか？

テストしている版と設定は[対応状況](compatibility.md)にあります。
