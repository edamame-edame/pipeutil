# レビュー集約（whole）

最終更新: 2026-03-11 07:35:00
最新レビューCSV: review/20260311071747.csv

## 1. 最新レビューの要約

- 未修正指摘数（Medium 以上）: 0
- 今回再レビューでの新規指摘（Medium 以上）: 0 件
- 修正確認済み指摘数: 64（R-064 まで集約済み）
- 今回対象: F-006 詳細設計 R-064 対応（`spec/F006_diagnostics_metrics.md`）

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
- R-045（Windows-IPC / F-004p2）: IOCP には `ReadFile` + OVERLAPPED + GQCS を使うこと。`ReadFileEx`（APC/alertable-wait 専用）との混用は禁止。
- R-046（Python-CAPI / F-004p2）: 外部 C++ スレッドから Python C API を呼ぶ際は `PyGILState_Ensure/Release` を使うこと。`Py_BEGIN/END_ALLOW_THREADS` は GIL 保持スレッド用であり dispatch thread での使用禁止。
- R-047（Timeout-Contract / F-004p2）: `receive(timeout_ms)` の timeout は native 側 `read_frame(timeout_ms)` へ透過渡しすること。Phase 1/2 でタイムアウト挙動を統一すること。
- R-048（Spec-Consistency / F-004p2）: `_pipeutil_async` は既存 `source/python/CMakeLists.txt` に統合し、`python_async` という独立サブディレクトリは作成しないこと。
- R-049（API-Contract / F-004p2）: `asyncio.wait_for` タイムアウトと IOCP 完了競合時は asyncio スレッド側で `if not future.done():` ガードを必ず入れること。
- R-050（Spec-Quality / F-004p2）: 仕様書本文の誤字（例:「使使用」）は実装前レビューで除去すること。
- R-051（Python-CAPI / F-004p2）: C-API サンプルでは `PyUnicode_FromString` 等の New reference を確実に解放し、Call 系 API の戻り値を必ず検査・解放すること。
- R-052（Concurrency-Lifecycle / F-004p2）: `do_close()` は `CancelIoEx` を先行し、dispatch thread が cancel 完了をドレインしてから停止センチネルで終了する順序を保つこと。
- R-053（Platform-Contract / F-004p2）: native backend 有効化は import 成否だけでなく実装完成度（プラットフォーム）で明示的にゲートすること。
- R-054（Python-CAPI / F-004p2）: C-API のメソッド呼び出しレシーバには必ず有効な `PyObject*` を渡し、null レシーバを禁止すること。
- R-055（API-Contract / F-004p2）: `message_id` は `uint32_t` へ変換前に範囲検証し、ラップアラウンドを防ぐこと。
- R-056（Python-CAPI / F-004p2）: `cancel()` の GIL 解放は実装分岐に合わせ、Python C-API を呼ぶ経路（Linux）では GIL を保持すること。
- R-057（API-Contract / F-003）: `ReconnectingRpcPipeClient` の `send/receive` は `RpcPipeClient` 契約（`send(msg)` / `receive(timeout: float)`）に合わせること。
- R-058（Spec-Quality / F-003）: 型スタブ例の誤記（`天常受信キュー`）は `通常受信キュー` に修正すること。
- R-059（API-Contract / F-003）: `AsyncReconnectingPipeClient.send` は `AsyncPipeClient` 契約どおり `send(msg)` とすること。
- R-060（Spec-Consistency / F-003）: §8.1 の `ReconnectingPipeClient` シグネチャ記述を本文 API と一致させること。
- R-061（Metrics-Contract / F-006）: `active_connections_` 前提の擬似コードを撤回し、`detach()` 運用と整合する累積統計バッファ方式へ更新したこと（ただし契約再整備は R-063 で継続）。
- R-062（Spec-Consistency / F-006）: `errors` カウントを `catch (const PipeException&)` に統一し、制約章の定義と一致させたこと。
- R-063（Metrics-Contract / F-006）: `SessionStats + active_stats_` 方式へ改め、`stats()` がアクティブ接続を含む全接続合算を返す契約を復元したこと。
- R-064（Concurrency-Contract / F-006）: `accumulated_mutex_` と `active_stats_mutex_` を `stats_mutex_` 1本に統合し、`SlotGuard::~SlotGuard()` が「active除去＋accumulated加算」を単一ロック下で原子的に実行するよう改め、`reset_stats()` との競合を完全排除したこと。

---

## 3. 未修正指摘（Medium 以上）

現在、未修正指摘はありません。

---

## 4. 再発監視（継続）

以下のカテゴリは今後の実装フェーズでも継続監視する。

- **Protocol-Format**: フレーム定義変更時は図・表・構造体・`static_assert` を同時に更新すること
- **Python-CAPI**: 新規 C 拡張関数追加時は所有権フローを必ずレビューに含めること
- **Windows-IPC**: Win32 API の境界ケース（`ERROR_*` の一覧）は実装前に完全列挙すること
- **Metrics-Contract**: 累積メトリクスは「現時点のアクティブ接続」ではなく「プロセス開始以降の累積値」を基本契約にし、接続終了で値が消えない設計を維持すること
