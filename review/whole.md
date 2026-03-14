# レビュー集約（whole）

最終更新: 2026-03-14 21:48:35
最新レビューCSV: review/20260314214835.csv

## 1. v1.0.0 最終総括

- 圧縮対象: review 配下 51 件の CSV と過去版 whole.md
- 最終判定: v1.0.0 はレビュー観点で出荷可能
- 未修正指摘数（Medium 以上）: 0
- 未修正指摘数（Low）: 0
- 修正確認済み指摘数: 86
- 最終検証: Windows / Linux とも Python 3.8-3.14 の全サポート版でビルド・wheel 生成・テスト完了

---

## 2. 圧縮レビュー結果

### 2.1 API / 仕様 / Python 公開面

- 公開 API 契約、型スタブ、docstring、実装例のズレを解消した。
- `Message` 入力型、再接続 client 群、`serve_connections()` 停止契約、`PipeAcl` 再公開、パッケージ内 import 規約を整合させた。
- 仕様書側では API 名、サンプル構文、バージョン表記、文字化けや誤記を整理し、実装と文書の齟齬を閉じた。

### 2.2 Python C API / 拡張モジュール安全性

- 参照カウント規約、GIL 規約、戻り値検査、buffer protocol、null レシーバ禁止など、C 拡張の基本安全性を収束した。
- Python 3.8 固有の `PyMODINIT_FUNC` visibility 問題を解消し、`PyInit__pipeutil` / `PyInit__pipeutil_async` の export 不全を修正した。
- これにより Linux cp38 wheel の import failure は解消済み。

### 2.3 Windows / POSIX プラットフォーム境界条件

- Windows named pipe の `ConnectNamedPipe` 分岐、`GetOverlappedResult`、ACL/SDDL 連携、`Advapi32` link を修正した。
- POSIX 側では `close(fd)` と `poll()` の競合による shutdown ハング、ディレクトリ権限補正、`lstat()` による symlink/非ディレクトリ拒否を実装した。
- クロスプラットフォーム差異は API と実装の両方で吸収され、出荷阻害要因は解消済み。

### 2.4 Async / 並行性 / メトリクス

- native async backend の read/write 完了通知、cancel、timeout 伝播、CRC、payload 上限、backend ゲートを収束した。
- 接続スロット解放、pending request の後始末、`stats()` の累積契約、単一ロック化を反映し、並行実行時の不整合を解消した。
- Linux native async backend の主要な use-after-free / 二重完了 / ハング要因は閉じている。

### 2.5 ビルド / パッケージング / テスト

- Windows と Linux の release wheel 生成経路を整備し、manylinux self-contained 化を完了した。
- Linux は Docker ベースの build/test 基盤を追加し、cp38-cp314 の manylinux wheel 生成まで自動化した。
- C++ / Python の追加テストを反映し、v1.0.0 時点のリリース検証は完了している。

---

## 3. 最終検証結果

- Windows wheel: cp38-cp314 の 7 本を生成済み
- Linux wheel: cp38-cp314 の 7 本を生成済み
- Windows pytest: 各バージョン 110 passed, 9 skipped
- Linux pytest: 各バージョン 113 passed, 6 skipped
- リリースブロッカー: なし

---

## 4. 今後の再発監視

- Protocol-Format: フレーム定義変更時は図・表・構造体・`static_assert` を同時更新すること
- Python-CAPI: 新規 C 拡張では所有権フロー、GIL、戻り値検査をレビュー項目に含めること
- Windows-IPC: `ERROR_*` 分岐と overlapped 完了パスを事前列挙すること
- POSIX-Lifecycle: `close` / `shutdown` / `poll` 相互作用を変更時に再確認すること
- Metrics-Contract: 累積統計が接続終了で消えない契約を維持すること

---

## 5. レビュー依頼用メモ

レビュー対象は v1.0.0 リリース一式。レビュー履歴は [review/20260314214835.csv](c:/Users/yuki/Desktop/program/pyproj/git/pipeutil/review/20260314214835.csv) と本ファイルに圧縮済み。現在の判定は「未修正指摘なし、全サポート Python / Windows / Linux で検証完了」のため、追加レビューは新規リスクの発見有無に集中すればよい。
