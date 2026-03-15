# レビュー集約（whole）

最終更新: 2026-03-15 13:44:00
最新レビューCSV: review/20260315134400.csv

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

---

## 6. v2.0.0 提案レビュー（暫定）

- 対象: `docs/feature_proposals_v2.0.md`
- 判定: 方向性は妥当だが、このままの Phase 1 着手は非推奨
- 主な論点:
	- ヘッダ v2 のビット配置が F-010 / F-013 / Section 6 で矛盾している
	- ストリーミングは protocol break 前に v1 互換で解ける範囲を先に評価すべき
	- protocol version は既存版の変更ではなく追加として扱い、公開は関連 feature 完了後に行うべき
	- Pub/Sub broker は core ではなく別パッケージ評価が妥当
	- Java wrapper は UDS と依存関係の前提が未確定
	- 並行 RPC / streaming には backpressure と cancellation 契約が不足している
- 詳細: `review/20260314224547.csv`, `review/v2.0.0_proposal_review.md`

---

## 7. v1.1.0 設計仕様レビュー総括

- 対象: `docs/design_change_spec_v1.1.0.md`, `spec/A001_capability_negotiation.md`, `spec/A002_flow_control.md`
- 最終更新: 2026-03-15
- 最終判定: 全指摘（V110×5、V111×3、V112×5、計 13 件）解消済み。実装着手を妨げる未解決指摘なし。
- 詳細: `review/20260315134400.csv`

### 初回仕様レビュー（V110-001〜005）

- **後退互換** (V110-001 High): v1.1.0 client → v1.0.0 server が非サポートであることを明示。A-001 Section 1 目的表を「サーバー先行アップグレード必須」に修正し、Section 4.4（非互換フロー）を新設。✅
- **canonical protocol 未更新** (V110-002 High): `spec/02_protocol.md` を 20B / version=0x02 / `message_id` フィールド追加で全面更新。✅
- **A-002 message_id 型** (V110-003 Medium): `cancel()` の message_id を `uint64_t` に変更し v1.3.0 計画と整合。✅
- **cancel 結果二重定義** (V110-004 Medium): `PipeErrorCode::Cancelled` に統一（`Interrupted` は `stop_accept()` 専用として区別）。✅
- **CRC-32C test vector 矛盾** (V110-005 Low): ファイル内では既に `0xAA36918A` のみに収束していた（追加修正なし）。✅

### HelloMode 設計レビュー（V111-001〜003、V112-001〜005）

HelloMode（Compat/Strict/Skip）の設計方針：「ユーザーファースト移行」——`Compat` をデフォルトとして移行タイミングをユーザー側へ委ねる。`Strict` はサーバー運用者が任意に選ぶ拒否ポリシー。

- **後退互換フロー再整備** (V111-001/002 High): `HelloMode::Compat` 方式（先頭 5B の version バイトで即時判別し v1.0.0 クライアントを v1-compat モードで受け入れ）を採用。A-001 Section 4.3・Section 4.5・design_change_spec Section 8.2/8.3 を全面整合。✅
- **旧サーバーの HELLO 扱い統一** (V111-003 Medium): 旧サーバーが `version=0x02` フレームを `InvalidMessage` で拒否するフローに A-001・design_change_spec を統一。✅
- **レビュー記録同期** (V112-001 High): 旧 Option B（fallback 削除）を最終解とした記録を現行 HelloMode 方針へ差し替え。本集約 CSV が最終記録となる。✅
- **HelloMode 権限分担明文化** (V112-002 High): サーバーの `HelloMode` が最終受け入れポリシーを決定。クライアント side は送信振る舞いのみ。A-001 Section 4.1・design_change_spec Section 8.0 に明記。✅
- **`NegotiatedCapabilities` v1_compat 追加** (V112-003 Medium): `v1_compat: bool` フィールドと `is_v1_compat()` を C++ 構造体・Python dataclass 双方に追加。`on_hello_complete` の全コールサイトを `caps={bitmap=..., v1_compat=...}` 形式に統一。✅
- **Skip モード HELLO 解釈統一** (V112-004 Medium): Skip サーバーは `FLAG_HELLO` を全フレームで無視（初回・2 回目以降を問わず `InvalidMessage` にしない）。A-001 Section 7 に例外を追記し、design_change_spec Section 8.5 の誤記述を修正。✅
- **`hello_timeout` 開始点統一** (V112-005 Medium): `accept()` 直後から計測・先頭 5 バイト read を含む定義を A-001 Section 4.5 と design_change_spec Section 8.0 の両方に明記。Skip はこのフローを通らないため Section 8.0 から除外。✅

