# レビュー集約（whole）

最終更新: 2026-02-28 02:12:28
最新レビューCSV: review/20260228021228.csv

## 1. 最新レビューの要約

- 未修正指摘数（Medium 以上）: 0
- 今回再レビューでの新規指摘（Medium 以上）: 0
- 修正確認済み指摘数: 23（詳細は削除し、注意点へ集約）
- ビルド確認:
  - CMake Tools（Visual Studio 17 2022 / Release）: ✅ ビルド成功
  - Python 3.13 wheel (`cp313-win_amd64`): ✅ 作成・インストール・import 確認済み
  - Python 3.14 wheel (`cp314-win_amd64`): ✅ 作成・インストール・import 確認済み
- GitHub 公開: ✅ https://github.com/edamame-edame/pipeutil.git（Initial commit: pipeutil v0.1.0）

---

## 2. 修正確認済み項目（注意点のみ）

- R-001（Spec-Inconsistency）: 仕様上限（4 GiB-1）と運用上限（2 GiB）を常に分離して記載すること。
- R-002（Protocol-Format）: フレーム図・定義表・構造体・`static_assert` の 4 点整合を保つこと。
- R-003（Naming-Contract）: 論理名と OS 実体名（Windows の `pipeutil_` 自動付与）を明確に分離すること。
- R-004（Python-CAPI）: 参照カウント規約（steal/非 steal）を API 単位で明示し、実装例と一致させること。
- R-005（Python-Packaging）: パッケージ内 import は相対 import を原則とすること。
- R-006（Windows-IPC）: `ConnectNamedPipe` の `ERROR_PIPE_CONNECTED` を成功ケースとして扱うこと。
- R-007（Error-Policy）: 公開 API のエラー契約は「例外中心」に統一し、戻り値コード混在を避けること。
- R-008（CrossPlatform-Compile）: エンディアン判定は `std::endian`（C++20 `<bit>`）を使い、GCC/Clang 固有マクロを使わないこと。
- R-009（Windows-IPC）: `ConnectNamedPipe` の分岐は `ERROR_PIPE_CONNECTED`（成功）/ `ERROR_IO_PENDING`（待機）/ その他（即時 SystemError）の三分岐を必ず設けること。
- R-010（Python-CAPI）: `PyModule_AddObjectRef` / `PyModule_AddStringConstant` の戻り値は必ずチェックし、失敗時は `Py_DECREF(m); return nullptr;` で後始末すること。
- R-011（Windows-IPC）: `ConnectNamedPipe` の分岐は `connected==TRUE`（即時成功）/ `ERROR_PIPE_CONNECTED`（成功）/ `ERROR_IO_PENDING`（待機）/ その他（即時 `SystemError`）の四分岐を必ず設けること。
- R-013（Windows-IPC）: `overlapped_write_all()` の `GetOverlappedResult` 戻り値は必ず検査し、失敗時は `map_win32_error` で変換して例外を送出すること（read 側と対称に）。
- R-014（Protocol-Format）: `Message::payload().size()` を `uint32_t` にキャストする前に `> numeric_limits<uint32_t>::max()` の overflow guard を入れること。
- R-015（Timeout-Contract）: `read_all` 系関数は関数入口でデッドラインを算出し、ループ内でチャンクごとの残り時間を計算して `poll` / `WaitForSingleObject` に渡すこと。
- R-016（Concurrency）: ハンドラ例外時でも `sem_.release()` / `active_count_` 更新を保証する RAII ガードでスロット枯渇を防ぐこと。
- R-017（Resource-Lifecycle）: `handler_threads_` の蓄積を避け、`detach()` + `active_count_` + `done_cv_` で完了追跡すること。
- R-018（Spec-Inconsistency）: acceptor のフロー記述を `server_accept_and_fork()` 前提に統一すること。
- R-019（Timeout-Contract）: `timeout=0` は `future.wait()`、有限値は `wait_for()` に分岐して契約どおり実装すること。
- R-020（Concurrency）: 応答照合は `pending_map_.find()` で存在確認後に完了処理し、未知 ID は破棄すること。
- R-021（API-Contract）: `serve_requests` 実行中の `receive()` / `send()` 直接呼び出しを禁止し、受信主体を単一化すること。
- R-022（Spec-Consistency）: 「公開 API 不変更」と「内部フレーム実装更新」の境界を設計書で明示すること。
- R-023（Spec-Consistency）: `RpcPipeServer` API 例の `stop()` 宣言を保持し、`run_in_background` と停止手段の契約を一致させること。
- R-024（Spec-Quality）: 設計書の文字化け/誤字を除去し、技術用語を一貫した日本語へ正規化すること。

---

## 3. 未修正指摘（Medium 以上）

なし（全件修正確認済み）

---

## 4. 再発監視（継続）

以下のカテゴリは今後の実装フェーズでも継続監視する。

- **Protocol-Format**: フレーム定義変更時は図・表・構造体・`static_assert` を同時に更新すること
- **Python-CAPI**: 新規 C 拡張関数追加時は所有権フローを必ずレビューに含めること
- **Windows-IPC**: Win32 API の境界ケース（`ERROR_*` の一覧）は実装前に完全列挙すること

