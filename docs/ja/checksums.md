# チェックサム

binlog のイベントは 4 バイトの CRC32 トレーラで終わることがあります。トレーラがあるかどうかはイベントごとではなくファイルの性質で、ファイルは FORMAT_DESCRIPTION イベント（FDE）でそれを宣言します。

## 方式の出どころ

各 binlog ファイルの先頭の FDE は、自身の 4 バイトのトレーラの直前の 1 バイトで方式を示します（`mybinlog/checksum.h`）。

| `ChecksumAlgorithm` | ワイヤ上のバイト | FDE 以降のイベント |
|---|---|---|
| `kOff` | 0 | トレーラなし。 |
| `kCrc32` | 1 | すべてのイベントが、それより前のバイトの CRC32 で終わります。 |
| `kUndefined` | 255（書かれない） | チェックサム以前のサーバー。方式のバイトもなく、FDE を含めどのイベントにもトレーラがありません。 |

チェックサムに対応したサーバーの FDE は、OFF を宣言するときでも自身のトレーラを必ず持ちます。MySQL 5.6.1 と MariaDB 5.3.0 より古いサーバーは、方式のバイトもトレーラも書きません。`ServerVersionPredatesChecksums()` が FDE のサーバー版からそれを見分けます。

方式は FDE でだけ変わり、次の FDE まで有効です。そのため、ファイルをまたぐストリームでは FDE のたびに方式を求め直します。方式をサーバー変数 `binlog_checksum` から取ることはありません。この変数は、以前の値で書かれた古いファイルをまだ読んでいる間にも変わりうるためです。

## デコーダの動作

デコーダは各イベントを有効な方式に従って切り出し、CRC32 ではトレーラを照合します。

- `DecoderOptions::verify_checksum = true`（既定）では、不一致は `kChecksumMismatch` で失敗します。トレーラを収めるには短すぎるチェックサム付きの枠も同じです。
- `verify_checksum = false` でも方式に従って枠を切り出すので、トレーラは本体から除かれますが、CRC は照合しません。別の層がバイトを検証済みのときに使います。
- `FormatDescriptionEvent::checksum_algorithm` は、各 FDE が宣言した方式を示します。
- 呼び出し側に渡すイベント本体（`UnknownEvent::body`、`TableRowsEvent::body`）にトレーラは含まれません。

レプリケーションストリームは、どの FDE よりも前の人工の ROTATE で始まります。この ROTATE は `DecoderOptions::default_checksum` で切り出すので、レプリカがソースに伝えた方式を設定します。OFF で切り出すとき、この ROTATE は CRC が一致した場合だけチェックサム付きとみなします。

TRANSACTION_PAYLOAD 内のイベントは、ファイルの方式にかかわらずトレーラを持ちません。

## ChecksumTracker

イベントを自分で切り出すコードは、デコーダと同じ部品である `ChecksumTracker` を使います。1 つのストリームの全イベントを順に渡します。

```cpp
mybinlog::ChecksumTracker tracker;  // assumes kCrc32 before the first FDE
mybinlog::FrameLayout layout;
mybinlog::Status status = tracker.OnEvent(header, frame, header.event_length, &layout);
if (status == mybinlog::Status::kOk) {
  const uint8_t* body = frame + mybinlog::kEventHeaderSize;
  size_t body_size = layout.body_length();  // trailer excluded
}
```

`OnEvent()` は、イベントが FDE なら方式を求め直してから枠を切り出します。`FrameLayout::payload_length` はヘッダと本体の長さで、`has_trailer` はトレーラが続くかどうかを示します。`SetDefaultAlgorithm()` は最初の FDE より前（と `Reset()` の後）に仮定する方式を設定し、`SetVerify(false)` は照合を省き、`algorithm()` は次の FDE 以外のイベントに有効な方式を返します。

`ReadFdeChecksumAlgorithm()` は、トラッカーを使わずに完全な FDE の枠から方式を読みます。トレーラの有無こそが判定の対象なので、枠全体を受け取ります。

トラッカーは 1 つのストリームに使い、同時には 1 つのスレッドからだけ使います。

## CRC32 の補助関数

`mybinlog/crc32.h` は、トレーラが使う CRC32（zlib の多項式）を公開しています。範囲には `ComputeCrc32(data, size)` を、途中の値を延長するには 0 から始めて `UpdateCrc32(crc, data, size)` を使います。
