// source/python/py_async_pipe.cpp
// AsyncPlatformPipe 実装: Windows IOCP + Linux epoll
// 仕様: spec/F004p2_async_native.md §3.3 / §3.4

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "py_async_pipe.hpp"
#include "py_debug_log.hpp"
#include "pipeutil/detail/frame_header.hpp"
#include "pipeutil/detail/endian.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace pipeutil::async {

// ──────────────────────────────────────────────────────────────────────────────
// CRC-32C (Castagnoli) — ローカル実装
// source/core/src/detail/crc32c.hpp は非公開パスのため再実装
// ──────────────────────────────────────────────────────────────────────────────
namespace {

template<std::size_t... I>
static constexpr std::array<uint32_t, 256>
make_crc32c_table_impl(std::index_sequence<I...>) noexcept {
    auto entry = [](uint32_t i) constexpr -> uint32_t {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
        return crc;
    };
    return {{ entry(static_cast<uint32_t>(I))... }};
}

static constexpr auto kCrc32cTable =
    make_crc32c_table_impl(std::make_index_sequence<256>{});

[[nodiscard]] static uint32_t crc32c_compute(
    const std::byte* data, std::size_t len) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = kCrc32cTable[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // anonymous namespace

// ──────────────────────────────────────────────────────────────────────────────
// Python グローバル変数（init_async_globals で初期化）
// ──────────────────────────────────────────────────────────────────────────────
// interned 文字列 — DECREF 不要（インタープリタ生存中は永続）
static PyObject* g_str_done                 = nullptr;
static PyObject* g_str_set_result           = nullptr;
static PyObject* g_str_set_exception        = nullptr;
static PyObject* g_str_call_soon_threadsafe = nullptr;
static PyObject* g_str_add_reader           = nullptr;
static PyObject* g_str_remove_reader        = nullptr;
static PyObject* g_str_add_writer           = nullptr;
static PyObject* g_str_remove_writer        = nullptr;

// _pipeutil から import した例外型（Borrowed ref、モジュール寿命で有効）
static PyObject* g_PipeError_type            = nullptr;
static PyObject* g_TimeoutError_type         = nullptr;
static PyObject* g_ConnectionResetError_type = nullptr;
static PyObject* g_BrokenPipeError_type      = nullptr;
static PyObject* g_InvalidMessageError_type  = nullptr;
static PyObject* g_TooManyConnectionsError_type = nullptr;  // _pipeutil_async で定義

// _fire_future 関数オブジェクト（init_async_globals でセット）
static PyObject* g_fire_future_func         = nullptr;
// Linux add_reader/writer コールバック（init_async_globals でセット）
static PyObject* g_on_readable_func         = nullptr;
static PyObject* g_on_writable_func         = nullptr;

// ──────────────────────────────────────────────────────────────────────────────
// 接続数管理 (R-044)
// ──────────────────────────────────────────────────────────────────────────────
static std::atomic<int> g_active_connections{0};

namespace {

/// RAII で接続カウントを管理する
class ConnectionGuard {
public:
    explicit ConnectionGuard(bool counted = false) noexcept : counted_(counted) {}

    [[nodiscard]] bool acquire() noexcept {
        const int prev = g_active_connections.fetch_add(1, std::memory_order_acq_rel);
        if (prev >= kMaxAsyncConnections) {
            g_active_connections.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        counted_ = true;
        return true;
    }

    void release() noexcept {
        if (counted_) {
            g_active_connections.fetch_sub(1, std::memory_order_acq_rel);
            counted_ = false;
        }
    }

    ~ConnectionGuard() noexcept { release(); }

    ConnectionGuard(const ConnectionGuard&)            = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;
    ConnectionGuard(ConnectionGuard&& o) noexcept : counted_(o.counted_)
        { o.counted_ = false; }
    ConnectionGuard& operator=(ConnectionGuard&& o) noexcept {
        if (this != &o) { release(); counted_ = o.counted_; o.counted_ = false; }
        return *this;
    }

private:
    bool counted_;
};

// ────────────────────────────────────────────────────────────────────────────
// Windows / Linux 共通: アクティブな非同期 I/O から Python future へ通知する
// ────────────────────────────────────────────────────────────────────────────

/// dispatch スレッドまたは asyncio スレッドから呼ぶ。
/// GIL を取得 → call_soon_threadsafe(g_fire_future_func, future, result) を発行。
/// loop_obj / future_obj の所有権を受け取り、DECREF する (R-039)。
static void schedule_fire_success(
    PyObject* loop_obj, PyObject* future_obj,
    const std::byte* data, std::size_t size) noexcept
{
    PyGILState_STATE gstate = PyGILState_Ensure();  // R-046

    PyObject* result = PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(data), static_cast<Py_ssize_t>(size));

    if (result) {
        PyObject* ret = PyObject_CallMethodObjArgs(
            loop_obj, g_str_call_soon_threadsafe,
            g_fire_future_func, future_obj, result,
            nullptr);
        if (!ret) {
            // RuntimeError = ループクローズ済み → クリアしてログのみ (R-043)
            PyErr_Clear();
            PIPELOG("schedule_fire_success: call_soon_threadsafe failed (loop closed?)");
        }
        Py_XDECREF(ret);
        Py_DECREF(result);
    } else {
        PyErr_Clear();  // MemoryError — 対処不可
    }

    Py_DECREF(future_obj);  // INCREF 対称 (R-039)
    Py_DECREF(loop_obj);    // INCREF 対称 (R-039)

    PyGILState_Release(gstate);
}

/// エラー時の future 通知。例外型を指定して set_exception をスケジュールする。
/// loop_obj / future_obj の所有権を受け取り、DECREF する (R-039)。
static void schedule_fire_error(
    PyObject* loop_obj, PyObject* future_obj,
    pipeutil::PipeErrorCode code, const char* what) noexcept
{
    PyGILState_STATE gstate = PyGILState_Ensure();

    // エラーコード → 例外型
    PyObject* exc_type = g_PipeError_type ? g_PipeError_type : PyExc_RuntimeError;
    switch (code) {
    case pipeutil::PipeErrorCode::Timeout:
        if (g_TimeoutError_type) exc_type = g_TimeoutError_type; break;
    case pipeutil::PipeErrorCode::ConnectionReset:
        if (g_ConnectionResetError_type) exc_type = g_ConnectionResetError_type; break;
    case pipeutil::PipeErrorCode::BrokenPipe:
        if (g_BrokenPipeError_type) exc_type = g_BrokenPipeError_type; break;
    case pipeutil::PipeErrorCode::InvalidMessage:
        if (g_InvalidMessageError_type) exc_type = g_InvalidMessageError_type; break;
    default: break;
    }

    PyObject* exc = PyObject_CallFunction(exc_type, "s", what ? what : "async I/O error");
    if (exc) {
        PyObject* ret = PyObject_CallMethodObjArgs(
            loop_obj, g_str_call_soon_threadsafe,
            g_fire_future_func, future_obj, exc,
            nullptr);
        if (!ret) {
            PyErr_Clear();  // R-043
            PIPELOG("schedule_fire_error: call_soon_threadsafe failed (loop closed?)");
        }
        Py_XDECREF(ret);
        Py_DECREF(exc);
    } else {
        PyErr_Clear();
    }

    Py_DECREF(future_obj);
    Py_DECREF(loop_obj);

    PyGILState_Release(gstate);
}

} // anonymous namespace

// ──────────────────────────────────────────────────────────────────────────────
// _fire_future 実装 (py_async_module.cpp から PyMethodDef で登録)
// schedule_fire_success / schedule_fire_error が call_soon_threadsafe でスケジュールする
// asyncio スレッド上で実行されるコールバック。
// ──────────────────────────────────────────────────────────────────────────────
PyObject* fire_future_impl(PyObject* /*module*/, PyObject* args) noexcept {
    PyObject* fut = nullptr;
    PyObject* val = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &fut, &val)) return nullptr;

    // done() チェックで InvalidStateError を防ぐ (R-049)
    PyObject* done_r = PyObject_CallMethodNoArgs(fut, g_str_done);
    if (!done_r) { PyErr_Clear(); Py_RETURN_NONE; }
    const int is_done = PyObject_IsTrue(done_r);
    Py_DECREF(done_r);
    if (is_done != 0) Py_RETURN_NONE;  // 既に done (cancelled / set) → スキップ

    if (PyExceptionInstance_Check(val)) {
        PyObject* ret = PyObject_CallMethodOneArg(fut, g_str_set_exception, val);
        if (!ret) PyErr_Clear();  // R-051: 戻り値チェック
        Py_XDECREF(ret);
    } else {
        PyObject* ret = PyObject_CallMethodOneArg(fut, g_str_set_result, val);
        if (!ret) PyErr_Clear();  // R-051: 戻り値チェック
        Py_XDECREF(ret);
    }
    Py_RETURN_NONE;
}

// ──────────────────────────────────────────────────────────────────────────────
// Windows IOCP 実装
// ──────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32

namespace {

using namespace pipeutil::detail;

/// GetLastError を PipeErrorCode へマッピング
static pipeutil::PipeErrorCode map_win32_error(DWORD err) noexcept {
    switch (err) {
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:           return pipeutil::PipeErrorCode::BrokenPipe;
    case ERROR_PIPE_NOT_CONNECTED:return pipeutil::PipeErrorCode::NotConnected;
    case ERROR_ACCESS_DENIED:     return pipeutil::PipeErrorCode::AccessDenied;
    case ERROR_SEM_TIMEOUT:
    case WAIT_TIMEOUT:            return pipeutil::PipeErrorCode::Timeout;
    case ERROR_OPERATION_ABORTED: return pipeutil::PipeErrorCode::Interrupted;
    default:                      return pipeutil::PipeErrorCode::SystemError;
    }
}

/// UTF-8 → UTF-16 変換 + pipeutil_ プレフィックス付き名前付きパイプパスを生成
static std::wstring to_pipe_wpath(const std::string& name) {
    const std::wstring prefix = L"\\\\.\\pipe\\pipeutil_";
    if (name.empty()) return prefix;
    const int len = MultiByteToWideChar(
        CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0);
    if (len <= 0) throw pipeutil::PipeException{
        pipeutil::PipeErrorCode::InvalidArgument, "Invalid pipe name encoding"};
    std::wstring ws(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()),
                        ws.data(), len);
    return prefix + ws;
}

// ─── IOCP キー定数 ─────────────────────────────────────────────────────────
constexpr ULONG_PTR kCloseKey = 0;  // dispatch_thread_ の停止シグナル

// ─── I/O コンテキスト（OVERLAPPED を先頭メンバーに置く） ────────────────────
enum class IoKind : uint8_t { Read, Write };

struct IoCtx {
    OVERLAPPED ov  = {};  // Must be first
    IoKind     kind;
    PyObject*  loop_obj   = nullptr;  // Py_INCREF 済み (R-039)
    PyObject*  future_obj = nullptr;  // Py_INCREF 済み (R-039)
};

struct ReadCtx : IoCtx {
    enum class Phase { Header, Payload } phase = Phase::Header;
    uint8_t                  hdr_raw[sizeof(FrameHeader)] = {};
    pipeutil::detail::FrameHeader hdr                     = {};
    std::vector<std::byte>   payload;

    ReadCtx() { kind = IoKind::Read; }
};

struct WriteCtx : IoCtx {
    std::vector<std::byte>   frame_buf;  // FrameHeader + payload 結合済み

    WriteCtx() { kind = IoKind::Write; }
};

} // anonymous namespace

// ──────────────────────────────────────────────────────────────────────────────
// AsyncPlatformPipe::Impl (Windows)
// ──────────────────────────────────────────────────────────────────────────────
struct AsyncPlatformPipe::Impl {
    HANDLE               hPipe_           = INVALID_HANDLE_VALUE;
    HANDLE               hIocp_           = nullptr;
    std::thread          dispatch_thread_;
    std::atomic<bool>    running_{ false };
    std::size_t          buf_size_;
    bool                 is_connected_    = false;
    ConnectionGuard      conn_guard_;
    std::wstring         pipe_wpath_;     // server_create_and_accept で再作成に使用

    // 同時発行可能な I/O は read × 1 + write × 1
    std::atomic<bool>    has_pending_read_{ false };
    std::atomic<bool>    has_pending_write_{ false };

    explicit Impl(std::size_t buf_size)
        : buf_size_(buf_size) {}

    ~Impl() { do_close(); }

    // ─── dispatch thread ────────────────────────────────────────────

    void start_dispatch() {
        running_.store(true, std::memory_order_release);
        dispatch_thread_ = std::thread([this]{ run_dispatch(); });
    }

    void run_dispatch() {
        PIPELOG("dispatch_thread: start");
        while (true) {
            DWORD     bytes = 0;
            ULONG_PTR key   = 0;
            OVERLAPPED* pov = nullptr;

            const BOOL ok = GetQueuedCompletionStatus(
                hIocp_, &bytes, &key, &pov, INFINITE);

            if (key == kCloseKey) {
                PIPELOG("dispatch_thread: close key → exit");
                break;
            }
            if (!pov) continue;

            auto* ctx = reinterpret_cast<IoCtx*>(pov);
            if (ctx->kind == IoKind::Read) {
                on_read_complete(static_cast<ReadCtx*>(ctx), ok, bytes);
            } else {
                on_write_complete(static_cast<WriteCtx*>(ctx), ok, bytes);
            }
        }
        PIPELOG("dispatch_thread: exit");
    }

    // ─── 読み取り完了ハンドラ ─────────────────────────────────────────

    void on_read_complete(ReadCtx* ctx, BOOL ok, DWORD /*bytes*/) noexcept {
        if (!ok) {
            const DWORD err = GetLastError();
            PIPELOG("on_read_complete: error=%lu", (unsigned long)err);
            has_pending_read_.store(false, std::memory_order_release);
            schedule_fire_error(ctx->loop_obj, ctx->future_obj,
                                map_win32_error(err), "async_read_frame failed");
            ctx->loop_obj = ctx->future_obj = nullptr;
            delete ctx;
            return;
        }

        if (ctx->phase == ReadCtx::Phase::Header) {
            // ヘッダ解析
            std::memcpy(&ctx->hdr, ctx->hdr_raw, sizeof(FrameHeader));

            if (std::memcmp(ctx->hdr.magic, MAGIC, 4) != 0 ||
                ctx->hdr.version != PROTOCOL_VERSION)
            {
                has_pending_read_.store(false, std::memory_order_release);
                schedule_fire_error(ctx->loop_obj, ctx->future_obj,
                    pipeutil::PipeErrorCode::InvalidMessage, "invalid frame header");
                ctx->loop_obj = ctx->future_obj = nullptr;
                delete ctx;
                return;
            }

            const uint32_t psz = from_le32(ctx->hdr.payload_size);
            if (psz > 0x7FFFFFFFu) {  // 運用上限 2 GiB - 1 (§8.2)
                has_pending_read_.store(false, std::memory_order_release);
                schedule_fire_error(ctx->loop_obj, ctx->future_obj,
                    pipeutil::PipeErrorCode::Overflow, "payload_size exceeds limit");
                ctx->loop_obj = ctx->future_obj = nullptr;
                delete ctx;
                return;
            }

            if (psz == 0) {
                // ペイロードなし → CRC チェックしてすぐ完了
                // 空ペイロードの checksum は 0 (§8.2)
                has_pending_read_.store(false, std::memory_order_release);
                schedule_fire_success(ctx->loop_obj, ctx->future_obj, nullptr, 0);
                ctx->loop_obj = ctx->future_obj = nullptr;
                delete ctx;
                return;
            }

            ctx->payload.resize(psz);
            ctx->phase = ReadCtx::Phase::Payload;

            // ペイロード読み取り開始
            ZeroMemory(&ctx->ov, sizeof(OVERLAPPED));
            const BOOL r = ReadFile(
                hPipe_, ctx->payload.data(), psz, nullptr, &ctx->ov);
            if (!r && GetLastError() != ERROR_IO_PENDING) {
                const DWORD err = GetLastError();
                has_pending_read_.store(false, std::memory_order_release);
                schedule_fire_error(ctx->loop_obj, ctx->future_obj,
                                    map_win32_error(err), "ReadFile (payload) failed");
                ctx->loop_obj = ctx->future_obj = nullptr;
                delete ctx;
            }
            // else: 非同期 I/O 開始 → dispatch_thread_ が再度通知を受け取る

        } else {
            // ペイロード受信完了 → CRC-32C 検証 (R-041)
            const uint32_t expected = from_le32(ctx->hdr.checksum);
            const uint32_t actual   = crc32c_compute(
                ctx->payload.data(), ctx->payload.size());

            if (expected != actual) {
                has_pending_read_.store(false, std::memory_order_release);
                schedule_fire_error(ctx->loop_obj, ctx->future_obj,
                    pipeutil::PipeErrorCode::InvalidMessage, "CRC-32C mismatch");
                ctx->loop_obj = ctx->future_obj = nullptr;
                delete ctx;
                return;
            }

            has_pending_read_.store(false, std::memory_order_release);
            schedule_fire_success(ctx->loop_obj, ctx->future_obj,
                                  ctx->payload.data(), ctx->payload.size());
            ctx->loop_obj = ctx->future_obj = nullptr;
            delete ctx;
        }
    }

    // ─── 書き込み完了ハンドラ ─────────────────────────────────────────

    void on_write_complete(WriteCtx* ctx, BOOL ok, DWORD /*bytes*/) noexcept {
        has_pending_write_.store(false, std::memory_order_release);
        if (!ok) {
            const DWORD err = GetLastError();
            PIPELOG("on_write_complete: error=%lu", (unsigned long)err);
            schedule_fire_error(ctx->loop_obj, ctx->future_obj,
                                map_win32_error(err), "async_write_frame failed");
        } else {
            schedule_fire_success(ctx->loop_obj, ctx->future_obj, nullptr, 0);
        }
        ctx->loop_obj = ctx->future_obj = nullptr;
        delete ctx;
    }

    // ─── 非同期 read 開始 ────────────────────────────────────────────

    void begin_async_read(PyObject* loop, PyObject* future) {
        if (has_pending_read_.exchange(true, std::memory_order_acq_rel)) {
            throw pipeutil::PipeException{
                pipeutil::PipeErrorCode::InvalidArgument,
                "async_read_frame: previous read still pending (logic error)"};
        }

        auto* ctx = new ReadCtx{};
        Py_INCREF(loop);    ctx->loop_obj   = loop;    // R-039
        Py_INCREF(future);  ctx->future_obj = future;  // R-039

        const BOOL r = ReadFile(
            hPipe_, ctx->hdr_raw, sizeof(FrameHeader), nullptr, &ctx->ov);
        if (!r && GetLastError() != ERROR_IO_PENDING) {
            const DWORD err = GetLastError();
            // I/O 発行失敗 → pending フラグを戻してエラー通知
            has_pending_read_.store(false, std::memory_order_release);
            schedule_fire_error(loop, future,
                                map_win32_error(err), "ReadFile (header) failed");
            ctx->loop_obj = ctx->future_obj = nullptr;  // DECREF は schedule_fire_error が行った
            delete ctx;
        }
    }

    // ─── 非同期 write 開始 ───────────────────────────────────────────

    void begin_async_write(const std::byte* data, std::size_t size,
                           uint32_t message_id, PyObject* loop, PyObject* future) {
        if (has_pending_write_.exchange(true, std::memory_order_acq_rel)) {
            throw pipeutil::PipeException{
                pipeutil::PipeErrorCode::InvalidArgument,
                "async_write_frame: previous write still pending (logic error)"};
        }

        // フレーム構築: FrameHeader + payload (R-041: CRC-32C は C++ が計算)
        const uint32_t psz = (size > 0xFFFFFFFFu)
            ? throw pipeutil::PipeException{pipeutil::PipeErrorCode::Overflow,
                                             "payload too large"}, 0u
            : static_cast<uint32_t>(size);
        const uint32_t crc = (size > 0) ? crc32c_compute(data, size) : 0u;

        auto* ctx = new WriteCtx{};
        ctx->frame_buf.resize(sizeof(FrameHeader) + size);

        FrameHeader hdr{};
        std::memcpy(hdr.magic, MAGIC, 4);
        hdr.version      = PROTOCOL_VERSION;
        hdr.flags        = 0;
        hdr.reserved[0]  = 0;
        hdr.reserved[1]  = 0;
        hdr.payload_size = to_le32(psz);
        hdr.checksum     = to_le32(crc);
        hdr.message_id   = to_le32(message_id);
        std::memcpy(ctx->frame_buf.data(), &hdr, sizeof(FrameHeader));
        if (size > 0) {
            std::memcpy(ctx->frame_buf.data() + sizeof(FrameHeader), data, size);
        }

        Py_INCREF(loop);    ctx->loop_obj   = loop;
        Py_INCREF(future);  ctx->future_obj = future;

        const BOOL r = WriteFile(
            hPipe_, ctx->frame_buf.data(),
            static_cast<DWORD>(ctx->frame_buf.size()),
            nullptr, &ctx->ov);
        if (!r && GetLastError() != ERROR_IO_PENDING) {
            const DWORD err = GetLastError();
            has_pending_write_.store(false, std::memory_order_release);
            schedule_fire_error(loop, future,
                                map_win32_error(err), "WriteFile failed");
            ctx->loop_obj = ctx->future_obj = nullptr;
            delete ctx;
        }
    }

    // ─── close ─────────────────────────────────────────────────────────

    void do_close() noexcept {
        if (running_.exchange(false, std::memory_order_acq_rel)) {
            // (R-052) CancelIoEx → PostQueuedCompletionStatus の順に実行する。
            // 逆順では kCloseKey センチネルが ERROR_OPERATION_ABORTED 完了通知より先に
            // IOCP へ到達し、dispatch thread がキャンセル完了をドレインせずに終了する。
            // その結果 ReadCtx/WriteCtx がリークし、Future が永久 hang する。
            if (hPipe_ != INVALID_HANDLE_VALUE) {
                CancelIoEx(hPipe_, nullptr);
            }
            if (hIocp_) {
                PostQueuedCompletionStatus(hIocp_, 0, kCloseKey, nullptr);
            }
        }
        // CloseHandle は dispatch thread 終了後に実行する（dispatch が IOCP から
        // 通知を受領中に handle を閉じると不定挙動になるため）。
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
        if (hPipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipe_);
            hPipe_ = INVALID_HANDLE_VALUE;
        }
        if (hIocp_) {
            CloseHandle(hIocp_);
            hIocp_ = nullptr;
        }
        is_connected_ = false;
        conn_guard_.release();
    }
};

// ──────────────────────────────────────────────────────────────────────────────
// AsyncPlatformPipe (Windows) メソッド実装
// ──────────────────────────────────────────────────────────────────────────────

AsyncPlatformPipe::AsyncPlatformPipe(std::size_t buf_size)
    : impl_(std::make_unique<Impl>(buf_size)) {}

AsyncPlatformPipe::~AsyncPlatformPipe() = default;

void AsyncPlatformPipe::client_connect(
    const std::string& pipe_name, int64_t timeout_ms)
{
    if (!impl_->conn_guard_.acquire()) {
        throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::TooManyConnections,
            "R-044: maximum async connections reached (64)"};
    }

    const std::wstring wpath = to_pipe_wpath(pipe_name);

    const auto deadline = (timeout_ms > 0)
        ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
        : std::chrono::time_point<std::chrono::steady_clock>::max();

    // Phase 1 (win32_pipe.cpp) と同様のリトライループ:
    // ERROR_PIPE_BUSY または ERROR_FILE_NOT_FOUND は WaitNamedPipeW + リトライ.
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    while (true) {
        hPipe = CreateFileW(
            wpath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);

        if (hPipe != INVALID_HANDLE_VALUE) break;

        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            impl_->conn_guard_.release();
            throw pipeutil::PipeException{map_win32_error(err), "CreateFileW failed"};
        }

        if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
            impl_->conn_guard_.release();
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout,
                                          "client_connect timed out"};
        }

        // 最大 10ms 待機してリトライ（サーバー起動待ち or PIPE_BUSY 解消待ち）
        WaitNamedPipeW(wpath.c_str(), 10);
    }

    // IOCP 作成・関連付け (R-045: ReadFile + OVERLAPPED + GQCS)
    HANDLE hIocp = CreateIoCompletionPort(hPipe, nullptr, 1, 1);
    if (!hIocp) {
        CloseHandle(hPipe);
        impl_->conn_guard_.release();
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                      "CreateIoCompletionPort failed"};
    }

    impl_->hPipe_ = hPipe;
    impl_->hIocp_ = hIocp;
    impl_->is_connected_ = true;
    impl_->start_dispatch();
}

std::unique_ptr<AsyncPlatformPipe>
AsyncPlatformPipe::server_create_and_accept(
    const std::string& pipe_name, int64_t timeout_ms)
{
    const std::wstring wpath = to_pipe_wpath(pipe_name);

    // === 初回呼び出し時: 自身のパイプを作成 ===
    if (impl_->hPipe_ == INVALID_HANDLE_VALUE) {
        impl_->pipe_wpath_ = wpath;

        HANDLE hPipe = CreateNamedPipeW(
            wpath.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            static_cast<DWORD>(impl_->buf_size_),
            static_cast<DWORD>(impl_->buf_size_),
            0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            throw pipeutil::PipeException{
                map_win32_error(GetLastError()), "CreateNamedPipeW failed"};
        }
        impl_->hPipe_ = hPipe;
    }

    // === クライアント接続待機 ===
    HANDLE hAcceptEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hAcceptEvent) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                      "CreateEventW (accept) failed"};
    }

    OVERLAPPED ov = {};
    ov.hEvent = hAcceptEvent;

    const BOOL connected = ConnectNamedPipe(impl_->hPipe_, &ov);
    const DWORD err      = GetLastError();

    if (!connected && err != ERROR_PIPE_CONNECTED && err != ERROR_IO_PENDING) {
        CloseHandle(hAcceptEvent);
        throw pipeutil::PipeException{map_win32_error(err), "ConnectNamedPipe failed"};
    }
    if (!connected && err == ERROR_IO_PENDING) {
        const DWORD dw_to = (timeout_ms <= 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
        const DWORD w = WaitForSingleObject(hAcceptEvent, dw_to);
        if (w == WAIT_TIMEOUT) {
            CancelIoEx(impl_->hPipe_, &ov);
            CloseHandle(hAcceptEvent);
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::Timeout,
                                          "server_create_and_accept timed out"};
        }
        if (w != WAIT_OBJECT_0) {
            CloseHandle(hAcceptEvent);
            throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                          "WaitForSingleObject failed"};
        }
    }
    CloseHandle(hAcceptEvent);

    // === 接続済み HANDLE を forked インスタンスへ移管 ===
    if (!impl_->conn_guard_.acquire()) {
        // 接続上限超過 → クライアントを切断して上限エラー (R-044)
        DisconnectNamedPipe(impl_->hPipe_);
        throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::TooManyConnections,
            "R-044: maximum async connections reached (64)"};
    }

    auto forked = std::make_unique<AsyncPlatformPipe>(impl_->buf_size_);
    forked->impl_->conn_guard_ = std::move(impl_->conn_guard_);
    impl_->conn_guard_ = ConnectionGuard{};  // リセット

    // forked インスタンスへ HANDLE + IOCP を渡す
    HANDLE hIocp = CreateIoCompletionPort(impl_->hPipe_, nullptr, 1, 1);
    if (!hIocp) {
        forked->impl_->conn_guard_.release();
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                      "CreateIoCompletionPort (forked) failed"};
    }
    forked->impl_->hPipe_        = impl_->hPipe_;
    forked->impl_->hIocp_        = hIocp;
    forked->impl_->is_connected_ = true;
    forked->impl_->start_dispatch();

    // 自身は次の accept のために INVALID 状態にリセットする。
    // 次回の server_create_and_accept 呼び出し時に再度 CreateNamedPipeW する。
    // （serve_connections パターンでは Python 側が新しいサーバーを作成するため
    //   ここでパイプを事前作成するとアバンドンドパイプが生まれ BrokenPipeError を引く。）
    impl_->hPipe_ = INVALID_HANDLE_VALUE;

    return forked;
}

void AsyncPlatformPipe::async_read_frame(PyObject* loop, PyObject* future) {
    impl_->begin_async_read(loop, future);
}

void AsyncPlatformPipe::async_write_frame(
    const std::byte* data, std::size_t size,
    uint32_t message_id, PyObject* loop, PyObject* future)
{
    impl_->begin_async_write(data, size, message_id, loop, future);
}

void AsyncPlatformPipe::cancel() noexcept {
    if (impl_->hPipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(impl_->hPipe_, nullptr);
        PIPELOG("cancel: CancelIoEx issued");
    }
}

void AsyncPlatformPipe::close() noexcept {
    impl_->do_close();
}

std::intptr_t AsyncPlatformPipe::native_handle() const noexcept {
    return reinterpret_cast<std::intptr_t>(impl_->hPipe_);
}

bool AsyncPlatformPipe::is_connected() const noexcept {
    return impl_->is_connected_;
}

#else  // ──────────────────────────────────────────────────────────────────────
// Linux POSIX 実装
// ──────────────────────────────────────────────────────────────────────────────

// LinuxReadCtx / LinuxWriteCtx が owner_impl フィールド（R-076）で参照するため
// anonymous namespace より前に前方宣言しておく。
struct AsyncPlatformPipe::Impl;

namespace {

/// UNIX ソケットパスを生成（/tmp/pipeutil_<name>.sock）
static std::string to_unix_socket_path(const std::string& name) {
    return "/tmp/pipeutil_" + name + ".sock";
}

// Linux 用 read/write コンテキスト（PyCapsule に格納してコールバックへ受け渡す）
struct LinuxReadCtx {
    int        fd;
    PyObject*  loop_obj;
    PyObject*  future_obj;
    uint8_t    hdr_raw[sizeof(pipeutil::detail::FrameHeader)] = {};
    pipeutil::detail::FrameHeader hdr = {};
    std::vector<std::byte> payload;
    std::size_t hdr_read     = 0;
    std::size_t payload_read = 0;
    bool        header_done  = false;
    // I/O 完了後に active_read_cap_ をクリアするためのバックポインタ（R-076）
    AsyncPlatformPipe::Impl* owner_impl = nullptr;

    LinuxReadCtx(int f, PyObject* l, PyObject* fut)
        : fd(f), loop_obj(l), future_obj(fut) {}

    ~LinuxReadCtx() {
        // Python オブジェクト DECREF は schedule_fire_* が行う。
        // schedule_fire_* を呼ばずに delete する場合は呼び出し元が DECREF する。
    }
};

struct LinuxWriteCtx {
    int        fd;
    PyObject*  loop_obj;
    PyObject*  future_obj;
    std::vector<std::byte> frame_buf;
    std::size_t written = 0;
    // I/O 完了後に active_write_cap_ をクリアするためのバックポインタ（R-076）
    AsyncPlatformPipe::Impl* owner_impl = nullptr;

    LinuxWriteCtx(int f, PyObject* l, PyObject* fut)
        : fd(f), loop_obj(l), future_obj(fut) {}
};

// capsule デストラクタ
static void linux_read_ctx_dealloc(PyObject* cap) {
    delete static_cast<LinuxReadCtx*>(PyCapsule_GetPointer(cap, "lrc"));
}
static void linux_write_ctx_dealloc(PyObject* cap) {
    delete static_cast<LinuxWriteCtx*>(PyCapsule_GetPointer(cap, "lwc"));
}

} // anonymous namespace

// ── on_linux_readable / on_linux_writable は py_async_module.cpp で
//    PyCFunction として定義し、g_on_readable_func / g_on_writable_func に設定する。

struct AsyncPlatformPipe::Impl {
    int   sock_fd_      = -1;
    bool  is_connected_ = false;
    std::size_t buf_size_;
    ConnectionGuard conn_guard_;

    // アクティブな読み取り・書き込み状態（asyncio スレッド上でのみアクセス）
    PyObject* active_read_cap_  = nullptr;  // LinuxReadCtx capsule
    PyObject* active_write_cap_ = nullptr;  // LinuxWriteCtx capsule

    explicit Impl(std::size_t buf_size) : buf_size_(buf_size) {}
    ~Impl() { do_close(); }

    void do_close() noexcept {
        if (sock_fd_ >= 0) {
            ::close(sock_fd_);
            sock_fd_ = -1;
        }
        is_connected_ = false;
        Py_CLEAR(active_read_cap_);
        Py_CLEAR(active_write_cap_);
        conn_guard_.release();
    }
};

AsyncPlatformPipe::AsyncPlatformPipe(std::size_t buf_size)
    : impl_(std::make_unique<Impl>(buf_size)) {}

AsyncPlatformPipe::~AsyncPlatformPipe() = default;

void AsyncPlatformPipe::client_connect(
    const std::string& pipe_name, int64_t timeout_ms)
{
    if (!impl_->conn_guard_.acquire()) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::TooManyConnections,
                                      "R-044: maximum async connections reached (64)"};
    }

    const std::string path = to_unix_socket_path(pipe_name);

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        impl_->conn_guard_.release();
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError,
                                      "socket() failed"};
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        impl_->conn_guard_.release();
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::NotFound,
                                      "connect() failed — pipe server not found"};
    }

    // O_NONBLOCK 設定
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    impl_->sock_fd_      = fd;
    impl_->is_connected_ = true;
}

std::unique_ptr<AsyncPlatformPipe>
AsyncPlatformPipe::server_create_and_accept(
    const std::string& pipe_name, int64_t /*timeout_ms*/)
{
    const std::string path = to_unix_socket_path(pipe_name);

    if (impl_->sock_fd_ < 0) {
        // サーバーソケット作成
        int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (srv < 0) throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::SystemError, "socket() failed"};

        ::unlink(path.c_str());
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(srv, 8) < 0)
        {
            ::close(srv);
            throw pipeutil::PipeException{
                pipeutil::PipeErrorCode::SystemError, "bind/listen failed"};
        }
        impl_->sock_fd_ = srv;
    }

    if (!impl_->conn_guard_.acquire()) {
        throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::TooManyConnections,
            "R-044: maximum async connections reached (64)"};
    }

    int cli = ::accept(impl_->sock_fd_, nullptr, nullptr);
    if (cli < 0) {
        impl_->conn_guard_.release();
        throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::SystemError, "accept() failed"};
    }

    int flags = ::fcntl(cli, F_GETFL, 0);
    ::fcntl(cli, F_SETFL, flags | O_NONBLOCK);

    auto forked = std::make_unique<AsyncPlatformPipe>(impl_->buf_size_);
    forked->impl_->sock_fd_      = cli;
    forked->impl_->is_connected_ = true;
    forked->impl_->conn_guard_   = std::move(impl_->conn_guard_);
    impl_->conn_guard_ = ConnectionGuard{};

    return forked;
}

void AsyncPlatformPipe::async_read_frame(PyObject* loop, PyObject* future) {
    if (impl_->active_read_cap_) {
        throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::InvalidArgument,
            "async_read_frame: previous read still active (logic error)"};
    }

    Py_INCREF(loop);
    Py_INCREF(future);
    auto* ctx = new LinuxReadCtx{impl_->sock_fd_, loop, future};
    ctx->owner_impl = impl_.get();
    impl_->active_read_cap_ = PyCapsule_New(ctx, "lrc", linux_read_ctx_dealloc);

    // loop.add_reader(fd, g_on_readable_func, capsule)
    PyObject* fd_obj = PyLong_FromLong(impl_->sock_fd_);
    PyObject* ret = PyObject_CallMethodObjArgs(
        loop, g_str_add_reader,
        fd_obj, g_on_readable_func, impl_->active_read_cap_,
        nullptr);
    Py_DECREF(fd_obj);
    if (!ret) {
        Py_CLEAR(impl_->active_read_cap_);
        // future/loop の DECREF は ctx が持っているが ctx は capsule が解放する
    }
    Py_XDECREF(ret);
}

void AsyncPlatformPipe::async_write_frame(
    const std::byte* data, std::size_t size,
    uint32_t message_id, PyObject* loop, PyObject* future)
{
    if (impl_->active_write_cap_) {
        throw pipeutil::PipeException{
            pipeutil::PipeErrorCode::InvalidArgument,
            "async_write_frame: previous write still active (logic error)"};
    }

    const uint32_t psz = (size > 0xFFFFFFFFu)
        ? throw pipeutil::PipeException{pipeutil::PipeErrorCode::Overflow, "payload too large"}, 0u
        : static_cast<uint32_t>(size);
    const uint32_t crc = (size > 0) ? crc32c_compute(data, size) : 0u;

    Py_INCREF(loop);
    Py_INCREF(future);
    auto* ctx = new LinuxWriteCtx{impl_->sock_fd_, loop, future};
    ctx->owner_impl = impl_.get();
    ctx->frame_buf.resize(sizeof(pipeutil::detail::FrameHeader) + size);

    pipeutil::detail::FrameHeader hdr{};
    std::memcpy(hdr.magic, pipeutil::detail::MAGIC, 4);
    hdr.version      = pipeutil::detail::PROTOCOL_VERSION;
    hdr.payload_size = pipeutil::detail::to_le32(psz);
    hdr.checksum     = pipeutil::detail::to_le32(crc);
    hdr.message_id   = pipeutil::detail::to_le32(message_id);
    std::memcpy(ctx->frame_buf.data(), &hdr, sizeof(pipeutil::detail::FrameHeader));
    if (size > 0) {
        std::memcpy(ctx->frame_buf.data() + sizeof(pipeutil::detail::FrameHeader),
                    data, size);
    }

    impl_->active_write_cap_ = PyCapsule_New(ctx, "lwc", linux_write_ctx_dealloc);

    PyObject* fd_obj = PyLong_FromLong(impl_->sock_fd_);
    PyObject* ret = PyObject_CallMethodObjArgs(
        loop, g_str_add_writer,
        fd_obj, g_on_writable_func, impl_->active_write_cap_,
        nullptr);
    Py_DECREF(fd_obj);
    if (!ret) { Py_CLEAR(impl_->active_write_cap_); }
    Py_XDECREF(ret);
}

void AsyncPlatformPipe::cancel() noexcept {
    // asyncio スレッド上で呼ばれる（GIL 保持中）

    // ── 読み取り側のキャンセル ───────────────────────────────────────────────
    if (impl_->active_read_cap_ && impl_->sock_fd_ >= 0) {
        // (R-054) loop_obj は LinuxReadCtx capsule から取得する。
        // nullptr を PyObject_CallMethodObjArgs に渡すと未定義動作によるクラッシュを引き起こす。
        auto* ctx = static_cast<LinuxReadCtx*>(
            PyCapsule_GetPointer(impl_->active_read_cap_, "lrc"));
        if (ctx && ctx->loop_obj) {
            PyObject* fd_obj = PyLong_FromLong(impl_->sock_fd_);
            if (fd_obj) {
                PyObject* ret = PyObject_CallMethodObjArgs(
                    ctx->loop_obj, g_str_remove_reader, fd_obj, nullptr);
                if (!ret) PyErr_Clear();
                Py_XDECREF(ret);
                Py_DECREF(fd_obj);
            }
        }
        Py_CLEAR(impl_->active_read_cap_);
    }

    // ── 書き込み側のキャンセル（R-078）─────────────────────────────────────
    if (impl_->active_write_cap_ && impl_->sock_fd_ >= 0) {
        auto* ctx = static_cast<LinuxWriteCtx*>(
            PyCapsule_GetPointer(impl_->active_write_cap_, "lwc"));
        if (ctx && ctx->loop_obj) {
            PyObject* fd_obj = PyLong_FromLong(impl_->sock_fd_);
            if (fd_obj) {
                PyObject* ret = PyObject_CallMethodObjArgs(
                    ctx->loop_obj, g_str_remove_writer, fd_obj, nullptr);
                if (!ret) PyErr_Clear();
                Py_XDECREF(ret);
                Py_DECREF(fd_obj);
            }
        }
        Py_CLEAR(impl_->active_write_cap_);
    }
}

void AsyncPlatformPipe::close() noexcept {
    impl_->do_close();
}

std::intptr_t AsyncPlatformPipe::native_handle() const noexcept {
    return static_cast<std::intptr_t>(impl_->sock_fd_);
}

bool AsyncPlatformPipe::is_connected() const noexcept {
    return impl_->is_connected_;
}

#endif // _WIN32

// ──────────────────────────────────────────────────────────────────────────────
// Linux ネイティブ I/O コールバック実装 (R-068)
// asyncio の SelectorEventLoop が add_reader / add_writer に渡す関数。
// GIL は asyncio スレッドが保持した状態で呼ばれる（R-046 に準拠）。
// ──────────────────────────────────────────────────────────────────────────────
#ifndef _WIN32

namespace {

/// readable 完了時: remove_reader → active_read_cap_ クリア → future に成功通知
static void linux_finish_read(
    LinuxReadCtx* ctx, const std::byte* data, std::size_t size) noexcept
{
    PyObject* loop = ctx->loop_obj;
    PyObject* fut  = ctx->future_obj;
    ctx->loop_obj   = nullptr;  // デストラクタでの二重 DECREF を防ぐ
    ctx->future_obj = nullptr;
    // asyncio ループから fd の監視を解除する
    PyObject* fd_obj = PyLong_FromLong(ctx->fd);
    if (fd_obj) {
        PyObject* r = PyObject_CallMethodObjArgs(
            loop, g_str_remove_reader, fd_obj, nullptr);
        if (!r) PyErr_Clear();
        Py_XDECREF(r);
        Py_DECREF(fd_obj);
    }
    // schedule_fire_success を先に呼ぶ（data は ctx->payload の内部ポインタのため、
    // ctx 解放前に PyBytes_FromStringAndSize でコピーする必要がある）。
    schedule_fire_success(loop, fut, data, size);
    // active_read_cap_ をクリアして再入ガードを解除する（R-076）
    // future への通知が完了した後で capsule を DECREF → delete ctx する。
    if (ctx->owner_impl) {
        PyObject* cap = ctx->owner_impl->active_read_cap_;
        ctx->owner_impl->active_read_cap_ = nullptr;
        Py_XDECREF(cap);  // → linux_read_ctx_dealloc → delete ctx
    }
}

/// readable エラー時: remove_reader → active_read_cap_ クリア → future に例外通知
static void linux_finish_read_err(
    LinuxReadCtx* ctx, pipeutil::PipeErrorCode code, const char* msg) noexcept
{
    PyObject* loop = ctx->loop_obj;
    PyObject* fut  = ctx->future_obj;
    ctx->loop_obj   = nullptr;
    ctx->future_obj = nullptr;
    PyObject* fd_obj = PyLong_FromLong(ctx->fd);
    if (fd_obj) {
        PyObject* r = PyObject_CallMethodObjArgs(
            loop, g_str_remove_reader, fd_obj, nullptr);
        if (!r) PyErr_Clear();
        Py_XDECREF(r);
        Py_DECREF(fd_obj);
    }
    // active_read_cap_ をクリアして再入ガードを解除する（R-076）
    // schedule_fire_error の msg は文字列リテラルのため解放後も安全だが、
    // 統一性のため schedule を先に呼ぶ。
    schedule_fire_error(loop, fut, code, msg);
    if (ctx->owner_impl) {
        PyObject* cap = ctx->owner_impl->active_read_cap_;
        ctx->owner_impl->active_read_cap_ = nullptr;
        Py_XDECREF(cap);  // → linux_read_ctx_dealloc → delete ctx
    }
}

/// writable 完了 / エラー時: remove_writer → active_write_cap_ クリア → future に通知
static void linux_finish_write(
    LinuxWriteCtx* ctx, bool success, const char* err_msg) noexcept
{
    PyObject* loop = ctx->loop_obj;
    PyObject* fut  = ctx->future_obj;
    ctx->loop_obj   = nullptr;
    ctx->future_obj = nullptr;
    PyObject* fd_obj = PyLong_FromLong(ctx->fd);
    if (fd_obj) {
        PyObject* r = PyObject_CallMethodObjArgs(
            loop, g_str_remove_writer, fd_obj, nullptr);
        if (!r) PyErr_Clear();
        Py_XDECREF(r);
        Py_DECREF(fd_obj);
    }
    // schedule_fire_* を先に呼ぶ（err_msg は文字列リテラルのため安全）。
    if (success) {
        schedule_fire_success(loop, fut, nullptr, 0);
    } else {
        schedule_fire_error(loop, fut, pipeutil::PipeErrorCode::BrokenPipe, err_msg);
    }
    // active_write_cap_ をクリアして再入ガードを解除する（R-076）
    if (ctx->owner_impl) {
        PyObject* cap = ctx->owner_impl->active_write_cap_;
        ctx->owner_impl->active_write_cap_ = nullptr;
        Py_XDECREF(cap);  // → linux_write_ctx_dealloc → delete ctx
    }
}

} // anonymous namespace

/// asyncio の add_reader コールバック: fd が readable なときに毎回呼ばれる。
/// ヘッダー→ペイロードを段階的に読み取り、全受信完了で future を resolve する。
PyObject* linux_on_readable_handler(PyObject* cap) noexcept {
    if (!PyCapsule_CheckExact(cap)) {
        PyErr_SetString(PyExc_TypeError, "expected LinuxReadCtx capsule");
        return nullptr;
    }
    auto* ctx = static_cast<LinuxReadCtx*>(PyCapsule_GetPointer(cap, "lrc"));
    if (!ctx) return nullptr;

    // ─── ヘッダー読み取りフェーズ ───────────────────────────────────
    if (!ctx->header_done) {
        const std::size_t remaining =
            sizeof(pipeutil::detail::FrameHeader) - ctx->hdr_read;
        const ssize_t n = ::read(
            ctx->fd,
            ctx->hdr_raw + ctx->hdr_read,
            remaining);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                Py_RETURN_NONE;  // データ未着 → 再待機
            }
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::BrokenPipe, "header read() failed");
            Py_RETURN_NONE;
        }
        if (n == 0) {
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::BrokenPipe, "connection closed (EOF on header)");
            Py_RETURN_NONE;
        }
        ctx->hdr_read += static_cast<std::size_t>(n);
        if (ctx->hdr_read < sizeof(pipeutil::detail::FrameHeader)) {
            Py_RETURN_NONE;  // ヘッダー部分受信 → 再待機
        }
        // ヘッダー完成: magic / version 検証
        std::memcpy(&ctx->hdr, ctx->hdr_raw, sizeof(ctx->hdr));
        if (std::memcmp(ctx->hdr.magic, pipeutil::detail::MAGIC, 4) != 0) {
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::InvalidMessage, "invalid frame magic");
            Py_RETURN_NONE;
        }
        if (ctx->hdr.version != pipeutil::detail::PROTOCOL_VERSION) {
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::InvalidMessage,
                "unsupported protocol version");
            Py_RETURN_NONE;
        }
        const uint32_t psz =
            pipeutil::detail::from_le32(ctx->hdr.payload_size);
        // R-077: Windows実装と同じ上限値で宣言不可能な大フレームを拒否する
        if (psz > 0x7FFFFFFFu) {
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::Overflow, "payload_size exceeds limit");
            Py_RETURN_NONE;
        }
        if (psz == 0) {
            // ゼロペイロード → 即完了
            linux_finish_read(ctx, nullptr, 0);
            Py_RETURN_NONE;
        }
        ctx->payload.resize(psz);
        ctx->header_done = true;
    }

    // ─── ペイロード読み取りフェーズ ─────────────────────────────────
    {
        const std::size_t remaining = ctx->payload.size() - ctx->payload_read;
        const ssize_t n = ::read(
            ctx->fd,
            reinterpret_cast<char*>(ctx->payload.data()) + ctx->payload_read,
            remaining);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                Py_RETURN_NONE;
            }
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::BrokenPipe, "payload read() failed");
            Py_RETURN_NONE;
        }
        if (n == 0) {
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::BrokenPipe,
                "connection closed (EOF on payload)");
            Py_RETURN_NONE;
        }
        ctx->payload_read += static_cast<std::size_t>(n);
        if (ctx->payload_read < ctx->payload.size()) {
            Py_RETURN_NONE;  // ペイロード部分受信 → 再待機
        }
    }

    // ─── CRC-32C 検証 ────────────────────────────────────────────────
    // R-077: checksum==0 のスキップを廃止。Windows 実装と同様、
    //        常に CRC を検証する（送信側も常に CRC を計算して送信する）。
    {
        const uint32_t expected_crc =
            pipeutil::detail::from_le32(ctx->hdr.checksum);
        const uint32_t actual_crc =
            crc32c_compute(ctx->payload.data(), ctx->payload.size());
        if (actual_crc != expected_crc) {
            linux_finish_read_err(
                ctx, pipeutil::PipeErrorCode::InvalidMessage, "CRC mismatch");
            Py_RETURN_NONE;
        }
    }

    // ─── 全受信完了 ──────────────────────────────────────────────────
    linux_finish_read(ctx, ctx->payload.data(), ctx->payload.size());
    Py_RETURN_NONE;
}

/// asyncio の add_writer コールバック: fd が writable なときに毎回呼ばれる。
/// frame_buf を残りバイト数ずつ書き込み、全送信完了で future を resolve する。
PyObject* linux_on_writable_handler(PyObject* cap) noexcept {
    if (!PyCapsule_CheckExact(cap)) {
        PyErr_SetString(PyExc_TypeError, "expected LinuxWriteCtx capsule");
        return nullptr;
    }
    auto* ctx = static_cast<LinuxWriteCtx*>(PyCapsule_GetPointer(cap, "lwc"));
    if (!ctx) return nullptr;

    const std::size_t remaining = ctx->frame_buf.size() - ctx->written;
    const ssize_t n = ::write(
        ctx->fd,
        ctx->frame_buf.data() + ctx->written,
        remaining);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            Py_RETURN_NONE;  // 送信バッファ満杯 → 再待機
        }
        linux_finish_write(ctx, false, "write() failed");
        Py_RETURN_NONE;
    }
    if (n == 0) {
        linux_finish_write(ctx, false, "connection closed (write returned 0)");
        Py_RETURN_NONE;
    }
    ctx->written += static_cast<std::size_t>(n);
    if (ctx->written < ctx->frame_buf.size()) {
        Py_RETURN_NONE;  // 部分送信 → 再待機
    }

    // ─── 全送信完了 ──────────────────────────────────────────────────
    linux_finish_write(ctx, true, nullptr);
    Py_RETURN_NONE;
}

#endif // !_WIN32

// ──────────────────────────────────────────────────────────────────────────────
// ──────────────────────────────────────────────────────────────────────────────
void set_async_pipe_exception(const pipeutil::PipeException& e) noexcept {
    PyObject* exc_type = nullptr;
    switch (e.pipe_code()) {
        case pipeutil::PipeErrorCode::Timeout:
            exc_type = g_TimeoutError_type; break;
        case pipeutil::PipeErrorCode::ConnectionReset:
            exc_type = g_ConnectionResetError_type; break;
        case pipeutil::PipeErrorCode::BrokenPipe:
            exc_type = g_BrokenPipeError_type; break;
        case pipeutil::PipeErrorCode::InvalidMessage:
            exc_type = g_InvalidMessageError_type; break;
        case pipeutil::PipeErrorCode::TooManyConnections:
            exc_type = g_TooManyConnectionsError_type; break;
        default:
            exc_type = g_PipeError_type; break;
    }
    if (!exc_type) exc_type = g_PipeError_type;
    if (!exc_type) exc_type = PyExc_RuntimeError;
    PyErr_SetString(exc_type, e.what());
}

// init_async_globals — PyInit__pipeutil_async から呼ぶ
// ──────────────────────────────────────────────────────────────────────────────
int init_async_globals(PyObject* module) noexcept {
    // インタープリタ生存中は解放しない interned strings
    g_str_done                 = PyUnicode_InternFromString("done");
    g_str_set_result           = PyUnicode_InternFromString("set_result");
    g_str_set_exception        = PyUnicode_InternFromString("set_exception");
    g_str_call_soon_threadsafe = PyUnicode_InternFromString("call_soon_threadsafe");
    g_str_add_reader           = PyUnicode_InternFromString("add_reader");
    g_str_remove_reader        = PyUnicode_InternFromString("remove_reader");
    g_str_add_writer           = PyUnicode_InternFromString("add_writer");
    g_str_remove_writer        = PyUnicode_InternFromString("remove_writer");

    if (!g_str_done || !g_str_set_result || !g_str_set_exception ||
        !g_str_call_soon_threadsafe) {
        return -1;
    }

    // _pipeutil から例外型を取得
    PyObject* pipeutil_mod = PyImport_ImportModule("pipeutil._pipeutil");
    if (pipeutil_mod) {
        g_PipeError_type            = PyObject_GetAttrString(pipeutil_mod, "PipeError");
        g_TimeoutError_type         = PyObject_GetAttrString(pipeutil_mod, "TimeoutError");
        g_ConnectionResetError_type = PyObject_GetAttrString(pipeutil_mod, "ConnectionResetError");
        g_BrokenPipeError_type      = PyObject_GetAttrString(pipeutil_mod, "BrokenPipeError");
        g_InvalidMessageError_type  = PyObject_GetAttrString(pipeutil_mod, "InvalidMessageError");
        Py_DECREF(pipeutil_mod);
        if (!g_PipeError_type) { PyErr_Clear(); }  // 取得失敗は RuntimeError フォールバック
    } else {
        PyErr_Clear();  // import 失敗 → フォールバック動作継続
    }

    // _fire_future 関数オブジェクトをモジュールから取得
    g_fire_future_func = PyObject_GetAttrString(module, "_fire_future");
    if (!g_fire_future_func) return -1;

    // Linux コールバックをモジュールから取得（Windows では使用しない）
#ifndef _WIN32
    g_on_readable_func = PyObject_GetAttrString(module, "_on_linux_readable");
    g_on_writable_func = PyObject_GetAttrString(module, "_on_linux_writable");
    if (!g_on_readable_func || !g_on_writable_func) return -1;
#endif

    // TooManyConnectionsError は _pipeutil_async モジュール自身に登録
    g_TooManyConnectionsError_type = PyObject_GetAttrString(
        module, "TooManyConnectionsError");
    if (!g_TooManyConnectionsError_type) PyErr_Clear();  // 未登録時は PipeError 使用

    return 0;
}

} // namespace pipeutil::async
