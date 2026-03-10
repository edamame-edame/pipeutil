// source/python/py_async_pipe.hpp
// AsyncPlatformPipe — OS 非同期 I/O 抽象レイヤー (Phase 2)
// 仕様: spec/F004p2_async_native.md §3.2
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "pipeutil/pipe_error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pipeutil::async {

// 接続数上限 (R-044): v0.5.0 では 64 接続まで
constexpr int kMaxAsyncConnections = 64;

// ──────────────────────────────────────────────────────────────────────────────
// AsyncPlatformPipe — OS 非同期 I/O 抽象レイヤー
//
// 使用順序:
//   client_connect()  または  server_create_and_accept()
//   → async_read_frame() / async_write_frame()  （1 件ずつ。並列不可）
//   → close()
//
// スレッドセーフ:
//   connect/close は呼び出し元スレッドから。
//   async_read_frame / async_write_frame は asyncio スレッドから。
// ──────────────────────────────────────────────────────────────────────────────
class AsyncPlatformPipe {
public:
    explicit AsyncPlatformPipe(std::size_t buf_size = 65536);
    ~AsyncPlatformPipe();

    // コピー・ムーブ禁止（HANDLE / fd 管理を単純化）
    AsyncPlatformPipe(const AsyncPlatformPipe&)            = delete;
    AsyncPlatformPipe& operator=(const AsyncPlatformPipe&) = delete;
    AsyncPlatformPipe(AsyncPlatformPipe&&)                 = delete;
    AsyncPlatformPipe& operator=(AsyncPlatformPipe&&)      = delete;

    // ─── 接続（同期: 呼び出し元スレッドでブロック） ──────────────────

    /// クライアントとして接続。GIL を解放して待機すること。
    /// 例外: PipeException (Timeout / NotFound / TooManyConnections / SystemError)
    void client_connect(const std::string& pipe_name, int64_t timeout_ms);

    /// サーバーとして受付・接続済みインスタンス返却（同期）。
    /// 自身は次クライアントを受け入れられる状態を維持する。
    /// 例外: PipeException (Timeout / TooManyConnections / SystemError)
    std::unique_ptr<AsyncPlatformPipe> server_create_and_accept(
        const std::string& pipe_name, int64_t timeout_ms);

    // ─── 非同期 I/O ──────────────────────────────────────────────────

    /// FrameHeader + payload を非同期に読み取る。
    /// 完了時 future.set_result(bytes) または future.set_exception(exc) をスケジュール。
    /// loop, future は呼び出し元が保持。本関数内で Py_INCREF する (R-039 対応)。
    void async_read_frame(PyObject* loop, PyObject* future);

    /// FrameHeader + payload を非同期に送信する。
    /// 完了時 future.set_result(None) または future.set_exception(exc) をスケジュール。
    void async_write_frame(const std::byte* data, std::size_t size,
                           uint32_t message_id, PyObject* loop, PyObject* future);

    // ─── キャンセル / クローズ ───────────────────────────────────────

    /// 実行中の非同期 I/O をキャンセルする（noexcept）
    /// Windows: CancelIoEx / Linux: remove_reader/remove_writer
    void cancel() noexcept;

    /// ハンドル / fd を閉じる（noexcept）
    void close() noexcept;

    // ─── 状態照会 ────────────────────────────────────────────────────

    [[nodiscard]] std::intptr_t native_handle() const noexcept;
    [[nodiscard]] bool          is_connected()  const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── モジュール初期化 ─────────────────────────────────────────────────────
// PyInit__pipeutil_async から呼ぶ。
// interned strings・例外型参照・g_fire_future_func 等を初期化する。
// 戻り値: 0 = 成功 / -1 = 失敗（Python 例外セット済み）
int init_async_globals(PyObject* module) noexcept;

/// PipeException を Python 例外に変換して PyErr_SetString する。
/// init_async_globals() でセット済みの例外型参照を使用する。
void set_async_pipe_exception(const pipeutil::PipeException& e) noexcept;

// _fire_future の PyMethodDef
// py_async_module.cpp で module method として登録し、
// g_fire_future_func に PyCFunction オブジェクトをセットする。
PyObject* fire_future_impl(PyObject* /*module*/, PyObject* args) noexcept;

} // namespace pipeutil::async
