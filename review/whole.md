# レビュー集約（whole）

最終更新: 2026-03-10 07:30:00
最新レビューCSV: review/20260310065000.csv

## 1. 最新レビューの要約

- 未修正指摘数（Medium 以上）: 0
- 今回再レビューでの新規指摘（Medium 以上）: 4 件（R-045〜R-048）→ 全件対応済み
- 修正確認済み指摘数: 43（詳細は削除し、注意点へ集約）
- 今回対象: F-004 Phase 2 仕様書レビュー（第2回、実装整合性観点）— 指摘対応完了

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
- R-029（Spec-Consistency / F-004）: `ProcessPipeServer` の接続モデルは docstring と内部フロー図を同一（spawn + `WorkerPipeClient` ハンドシェイク）に統一すること。
- R-030（API-Contract / F-004）: `serve_connections()` の停止契約は `stop_event` をシグネチャに含め、docstring/例と一致させること。
- R-031（Platform-Contract / F-004）: Windows 章では `fork` 前提を排除し、`subprocess.Popen(spawn) + DuplicateHandle` に統一すること。
- R-032（Spec-Inconsistency / F-004）: Python サポート範囲はプロジェクト基準（3.8-3.14）と仕様本文（バージョン表・コード例・フォールバック）を常に一致させること。
- R-033（Spec-Quality / F-004）: API 名・サンプル構文（`CancelIoEx` / `hasattr(asyncio, "to_thread")`）を実行可能な記法に維持すること。
- R-034（Python-Packaging / F-005）: パッケージ内実装例の import は `from ._pipeutil import Message` に統一し、循環依存リスクを避けること。
- R-035（Spec-Completeness / F-005）: テスト例で参照する補助関数（`_msgpack_unavailable()`）はサンプル内に定義を含めること。
- R-036（Type-Contract / F-005）: `__init__.pyi` 追記時は `CodecError(PipeError)` を例外階層定義後に配置すること。
- R-037（Spec-Quality / F-005）: 誤記（例: メモリ用語）を除去し、仕様の可読性を維持すること。
- R-038（Concurrency/Lifecycle / F-004p2）: dispatch thread の start（connect）/stop（close で join）タイミングを必ず明示すること。
- R-039（Python-CAPI / F-004p2）: dispatch thread → asyncio コールバック時の PyObject*（loop, future）に INCREF/DECREF を対称に施すこと。
- R-040（API-Contract / F-004p2）: server_create_and_accept の unique_ptr → PyAsyncPipeHandle ラップ手順（tp_alloc / release / エラー時後始末）を C 拡張実装前に確定すること。
- R-041（Protocol-Correctness / F-004p2）: async_write_frame は C++ 内部で CRC-32C を計算し FrameHeader.checksum に嵌めて送信すること。
- R-042（Platform-Contract / F-004p2）: add_reader/add_writer は SelectorEventLoop 専用。Windows の ProactorEventLoop では動作しない旨を必ず文書化すること。
- R-043（Error-Policy / F-004p2）: call_soon_threadsafe が RuntimeError を送出した場合（ループクローズ済み）は PyErr_Clear + PIPELOG で処理すること。
- R-044（Resource-Policy / F-004p2）: dispatch thread の上限数（64 接続ス v0.5.0）を数値として明示し、超過時は TooManyConnections を送出すること。

---

## 3. 未修正指摘（Medium 以上）

### R-045（Critical / Windows-IPC / F-004p2）
**指摘**: IOCP モデル説明で `ReadFileEx(..., OVERLAPPED, nullptr)` と `GetQueuedCompletionStatus` を同時利用しているが API 契約上非整合。IOCP で使うべきは `ReadFile/WriteFile` + OVERLAPPED（Ex 系は APC/alertable wait 前提）。
**影響**: 設計どおり実装すると完了通知を受け取れず読み書きがハングまたは未完了になる。
**根拠**: Win32 API 契約（ReadFileEx は completion routine + alertable wait、IOCP は GQCS で回収）。
**対応状況**: 未対応
→ spec/F004p2_async_native.md §3.3 の IOCP ダイアグラムおよびフロー説明を `ReadFile(hPipe_, buf, N, nullptr, &ov_)` + GQCS 構成に全面修正。ReadFileEx の記述を削除。✅

### R-046（High / Python-CAPI / F-004p2）
**指摘**: 「dispatch thread 内で `Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS` を適切に扱う」とあるがこのマクロは“現在 GIL を保持しているスレッド”向け。純粋 C++ スレッドから Python C API を触る契約として不適切。
**影響**: 誤実装時に未定義動作（クラッシュ/UAF/デッドロック）を招く。
**根拠**: CPython C-API の GIL 契約（外部スレッドは `PyGILState_Ensure/Release` で入る）。
**対応状況**: 未対応
→ spec/F004p2_async_native.md §3.1 設計原則4を全面改訂。`Py_BEGIN/END_ALLOW_THREADS` の記述を削除し、dispatch thread は GIL 非保持の純粋 C++ スレッドであること・Python C API 呼び出し前に `PyGILState_Ensure()` / 完了後に `PyGILState_Release()` を使うことを明記。§3.3 ダイアグラムにも呼び出し手順を追記。✅

### R-047（Medium / Timeout-Contract / F-004p2）
**指摘**: `AsyncPipeClient.receive(timeout_ms=...)` の公開契約に対し native 側 `read_frame()` にタイムアウト入力が存在しない。`aio.py` 切り替え後に timeout_ms が実質無視される設計になっている。
**影響**: Phase 1 と Phase 2 でタイムアウト挙動が不一致となり API 互換を破る。
**根拠**: F-004 の既存 API 契約（timeout 引数）および「Phase 1/2 同一 API」方針。
**対応状況**: 未対応
→ spec/F004p2_async_native.md §5.2 `NativeAsyncPipe.read_frame()` に `timeout_ms: int = 0` 引数を追加。0=無制限待機、正値は `asyncio.wait_for` でラップして `pipeutil.TimeoutError` へ変換するコード例を追記。§6.2 `AsyncPipeClient.receive()` の native パスも `read_frame(timeout_ms)` に修正し timeout が透過されることを明示。✅

### R-048（Medium / Spec-Consistency / F-004p2）
**指摘**: `source/CMakeLists.txt` 例で `add_subdirectory(python_async)` を参照しているが、現リポジトリ構成は `source/python/`。本文と実際のディレクトリ構成が不一致。
**影響**: 実装者が誤ったサブディレクトリ追加を行いビルド設定が破綻する。
**根拠**: ワークスペース実構成（`source/python/CMakeLists.txt`）との整合要件。
**対応状況**: 未対応
→ spec/F004p2_async_native.md §7.1 の `add_subdirectory(python_async)` を削除し、既存 `source/python/CMakeLists.txt` に `_pipeutil_async` ターゲットを追加する形（§7.2 参照）に変更。コメントで理由を明記。✅

---

## 4. 再発監視（継続）

以下のカテゴリは今後の実装フェーズでも継続監視する。

- **Protocol-Format**: フレーム定義変更時は図・表・構造体・`static_assert` を同時に更新すること
- **Python-CAPI**: 新規 C 拡張関数追加時は所有権フローを必ずレビューに含めること
- **Windows-IPC**: Win32 API の境界ケース（`ERROR_*` の一覧）は実装前に完全列挙すること

