# レビュー集約（whole）

最終更新: 2026-03-08 12:35:32
最新レビューCSV: review/20260308123532.csv

## 1. 最新レビューの要約

- 未修正指摘数（Medium 以上）: 4
- 今回再レビューでの新規指摘（Medium 以上）: 4
- 修正確認済み指摘数: 27（詳細は削除し、注意点へ集約）
- ビルド確認:
  - CMake Tools（Visual Studio 17 2022 / Release）: ✅ ビルド成功
  - Python 3.14 wheel (`cp314-win_amd64`): ✅ ビルド成功
- テスト結果（F-002 実装後）:
  - C++ (CTest): ✅ 45/45 PASS
  - Python (pytest): ✅ 25/25 PASS
- GitHub: ✅ https://github.com/edamame-edame/pipeutil.git（c30e0fd: docs(F-004)）

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
- R-025（Windows-IPC / F-001）: `server_close()` の `DisconnectNamedPipe` は forked 接続では不要であり、クライアント側 pending I/O を即座に失敗させる。`FlushFileBuffers + CloseHandle` に変更すること。
- R-026（Python-CAPI / F-001）: `PyMessage_Type.tp_as_buffer = nullptr` のままでは `bytes(msg)` が TypeError。`PyBufferProcs` を実装して buffer protocol を有効化すること。
- R-027（Python-CAPI / F-001）: `to_ms(double sec)` が入力をミリ秒ではなく秒と誤解釈して `* 1000.0` していた。Python API のタイムアウト引数はミリ秒単位なので変換不要（`static_cast<int64_t>(ms_val)` のみ）。
- R-028（API-Contract / F-001）: `PyMessage_init` が `str` 入力を受け付けていたが、公開 API は `bytes / bytearray` のみを要求する。`PyUnicode_Check` ブランチを削除し `TypeError` を送出すること。

---

## 3. 未修正指摘（Medium 以上）

### R-029（High, Spec-Consistency）

**指摘**: `ProcessPipeServer` の設計説明が二重化しており、接続モデルが矛盾している（接続転送を使う設計 vs `WorkerPipeClient` が同一 `pipe_name` に直接接続）。
→ `ProcessPipeServer` の docstring を「ハンドシェイクパイプパターン」に統一し、「WorkerPipeClient 経由にしない」という記述を削除。§5.2 の内部動作フローを §5.3 のフロー図と一致させた。✅

**根拠**: [spec/F004_async_threading_multiprocessing.md](../spec/F004_async_threading_multiprocessing.md#L520-L525) では「WorkerPipeClient 経由にしない」と記載する一方、[spec/F004_async_threading_multiprocessing.md](../spec/F004_async_threading_multiprocessing.md#L592-L608) では `WorkerPipeClient("my_pipe_worker_N").connect()` を要求している。

**対応状況**: 未修正

### R-030（Medium, API-Contract）

**指摘**: `serve_connections` の docstring が `stop_event` 前提なのに関数シグネチャに停止手段が存在しない。
→ `stop_event: asyncio.Event | None = None` を `serve_connections()` の引数に追加し、docstring と使用例も `stop_event=stop` を渡す形に更新した。✅

**根拠**: [spec/F004_async_threading_multiprocessing.md](../spec/F004_async_threading_multiprocessing.md#L313-L323) で「`stop_event` が set されるまで動き続ける」とあるが、関数引数に `stop_event` がない。

**対応状況**: 未修正

### R-031（Medium, Platform-Contract）

**指摘**: Windows セクションで「接続→fork→子プロセス引継ぎ」を可能と記述しているが、Windows には `fork` がない。
→ §5.4 の記述を「接続 → subprocess.Popen (spawn) → DuplicateHandle で子プロセスへ引き継ぐ」に修正し、「Windows には POSIX fork がないため DuplicateHandle + スポーン方式を使う」旨を明記した。✅

**根拠**: [spec/F004_async_threading_multiprocessing.md](../spec/F004_async_threading_multiprocessing.md#L661-L664) の本文が `fork` 前提になっている。

**対応状況**: 未修正

### R-032（Medium, Spec-Inconsistency）

**指摘**: プロジェクト基準（Python3.13）と異なり、仕様が Python 3.9 以上/3.8 フォールバックを前提にしている。
→ §6.1 テーブルを Python 3.13 前提で書き直し、3.8/3.9 フォールバック記述を削除。目標バージョンを「Python 3.13 以上」へ変更、CI 表記も更新。§9 の `aio.py` 冒頭コードも `(3, 13)` チェックに修正した。✅

**根拠**: [AGENTS.md](../AGENTS.md) の基準と、[spec/F004_async_threading_multiprocessing.md](../spec/F004_async_threading_multiprocessing.md#L697-L704) および [spec/F004_async_threading_multiprocessing.md](../spec/F004_async_threading_multiprocessing.md#L875-L883) の記述が不一致。

**対応状況**: 未修正

---

## 4. 再発監視（継続）

以下のカテゴリは今後の実装フェーズでも継続監視する。

- **Protocol-Format**: フレーム定義変更時は図・表・構造体・`static_assert` を同時に更新すること
- **Python-CAPI**: 新規 C 拡張関数追加時は所有権フローを必ずレビューに含めること
- **Windows-IPC**: Win32 API の境界ケース（`ERROR_*` の一覧）は実装前に完全列挙すること

