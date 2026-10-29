# 用語集

**人工のイベント（artificial event）。** ソースがファイルから読むのではなく、レプリケーションストリームのために作るイベントです。ダンプの先頭の ROTATE などがこれにあたります。ヘッダの `kLogEventArtificialFlag` が立ち、`next_position` は 0 です。

**binlog の位置。** binlog ファイル内のイベントのバイトオフセットです。最初のイベントはマジックナンバーの後ろの 4 にあります。`EventHeader::next_position` は次のイベントの位置です。

**共通ヘッダ。** すべてのイベントの先頭にある 19 バイトで、タイムスタンプ、型コード、サーバー ID、イベントの長さ、次の位置、フラグを持ちます。

**扱い（disposition）。** 読み手がイベントの型に対して行うこと。デコードする、データに影響しないものとして読み飛ばす、止まる、のいずれかです。[イベントの扱い](disposition.md)を参照してください。

**FDE（FORMAT_DESCRIPTION イベント）。** すべての binlog ファイルでマジックナンバーの次にあるイベントです。サーバーの版を示し、ファイルの残りのチェックサム方式を宣言します。

**サーバーの種類（flavor）。** MySQL か MariaDB か。両者は binlog の枠組みを共有しますが、イベントの番号、GTID、圧縮、一部のメタデータの規則が異なります。

**GNO。** MySQL の GTID のトランザクション番号です。

**GTID。** グローバルトランザクション ID。MySQL では `uuid[:tag]:gno`、MariaDB では `domain-server-seq` です。[GTID](gtids.md) を参照してください。

**PARTIAL_JSON。** MySQL の設定 `binlog_row_value_options=PARTIAL_JSON` で、JSON の更新を編集の列としてログに書けるようにします。[JSON](json.md) を参照してください。

**行イメージ。** 行イベントが運ぶ 1 行分の列の値です。UPDATE には変更前イメージと変更後イメージがあります。イメージが持つ列は `binlog_row_image`（FULL、MINIMAL、NOBLOB）で決まります。

**行メタデータ。** `binlog_row_metadata` で制御される TABLE_MAP の任意フィールドです。符号の有無、文字セット、列名、ENUM と SET のラベルがあります。

**行イベント。** WRITE_ROWS、UPDATE_ROWS、DELETE_ROWS、PARTIAL_UPDATE_ROWS。1 つのテーブルの、1 つの文（またはその一部）の行イメージです。

**テーブル ID。** TABLE_MAP がテーブルに割り当て、行イベントが参照する番号です。定義したファイルの中でだけ有効です。

**TABLE_MAP。** テーブルを変更する行イベントの前に、そのテーブル（スキーマ、名前、列の型とメタデータ）を記述するイベントです。

**トレーラ。** ファイルのチェックサム方式が CRC32 のとき、イベントの末尾にある 4 バイトの CRC32 です。

**TRANSACTION_PAYLOAD。** `binlog_transaction_compression=ON` のとき、トランザクション全体を zstd で圧縮して運ぶ MySQL のイベントです。

**TSID。** トランザクションのソースの識別子。MySQL のサーバー UUID と任意のタグの組です。
