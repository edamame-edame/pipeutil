// rpc_pipe_server.cpp — RpcPipeServer pimpl 実装
// 仕様: spec/F002_rpc_message_id.md §5, §7
#include "pipeutil/rpc_pipe_server.hpp"
#include "pipeutil/pipe_acl.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/pipe_stats.hpp"
#include "detail/frame_io.hpp"
#include "detail/platform_factory.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace pipeutil {

// ─────────────────────────────────────────────────────────────────────────────
// RpcPipeServer::Impl
// ─────────────────────────────────────────────────────────────────────────────
class RpcPipeServer::Impl {
public:
    Impl(std::string name, std::size_t buf_size)
        : pipe_name_(std::move(name))
        , platform_(std::make_unique<detail::PlatformPipeImpl>(buf_size))
    {}

    ~Impl() { close(); }

    // ─── ライフサイクル ──────────────────────────────────────────────

    void listen() {
        // RpcPipeServer は ACL カスタマイズ非対応; Default を使用 (F-008 後方互換)
        platform_->server_create(pipe_name_, PipeAcl::Default, "");
    }

    void accept(std::chrono::milliseconds timeout) {
        platform_->server_accept(timeout.count());
    }

    void close() noexcept {
        stop();    // 背景スレッドがあれば停止
        platform_->server_close();
    }

    // ─── RPC サーバーループ ───────────────────────────────────────────

    void serve_requests(RequestHandler handler, bool run_in_background) {
        stop_flag_.store(false, std::memory_order_release);
        is_serving_.store(true, std::memory_order_release);

        if (run_in_background) {
            handler_thread_ = std::thread([this, h = std::move(handler)] {
                serve_loop(h);
            });
        } else {
            // ブロッキング: 呼び出し元スレッドで実行
            serve_loop(handler);
        }
    }

    void stop() noexcept {
        stop_flag_.store(true, std::memory_order_release);
        if (handler_thread_.joinable()) {
            handler_thread_.join();
        }
        is_serving_.store(false, std::memory_order_release);
    }

    // ─── 通常 send / receive (serve_requests 前にのみ使用) ─────────────────

    void send(const Message& msg) {
        std::lock_guard lk{io_mutex_};
        try {
            detail::send_frame(*platform_, msg);
            stat_msgs_sent_.fetch_add(1, std::memory_order_relaxed);
            stat_bytes_sent_.fetch_add(msg.size(), std::memory_order_relaxed);
        } catch (const PipeException&) {
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            throw;
        }
    }

    Message receive(std::chrono::milliseconds timeout) {
        std::lock_guard lk{io_mutex_};
        try {
            auto fr = detail::recv_frame(*platform_, timeout.count());
            stat_msgs_recv_.fetch_add(1, std::memory_order_relaxed);
            stat_bytes_recv_.fetch_add(fr.message.size(), std::memory_order_relaxed);
            return fr.message;
        } catch (const PipeException&) {
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            throw;
        }
    }

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool               is_listening() const noexcept { return platform_->is_server_listening(); }
    [[nodiscard]] bool               is_connected() const noexcept { return platform_->is_connected(); }
    [[nodiscard]] bool               is_serving()   const noexcept { return is_serving_.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& pipe_name()    const noexcept { return pipe_name_; }

    [[nodiscard]] PipeStats stats_snapshot() const noexcept {
        PipeStats s;
        s.messages_sent     = stat_msgs_sent_.load(std::memory_order_relaxed);
        s.messages_received = stat_msgs_recv_.load(std::memory_order_relaxed);
        s.bytes_sent        = stat_bytes_sent_.load(std::memory_order_relaxed);
        s.bytes_received    = stat_bytes_recv_.load(std::memory_order_relaxed);
        s.errors            = stat_errors_.load(std::memory_order_relaxed);
        return s;
    }

    void reset_stats() noexcept {
        stat_msgs_sent_.store(0, std::memory_order_relaxed);
        stat_msgs_recv_.store(0, std::memory_order_relaxed);
        stat_bytes_sent_.store(0, std::memory_order_relaxed);
        stat_bytes_recv_.store(0, std::memory_order_relaxed);
        stat_errors_.store(0, std::memory_order_relaxed);
    }

private:
    using RequestHandler = RpcPipeServer::RequestHandler;

    std::string                            pipe_name_;
    std::unique_ptr<detail::IPlatformPipe> platform_;
    std::mutex                             io_mutex_;  // send / serve_loop 内送信の排他

    std::thread                            handler_thread_;
    std::atomic<bool>                      stop_flag_{false};
    std::atomic<bool>                      is_serving_{false};

    // ─ 統計カウンタ (F-006) ────────────────────────────────────────────
    std::atomic<uint64_t> stat_msgs_sent_{0};
    std::atomic<uint64_t> stat_msgs_recv_{0};
    std::atomic<uint64_t> stat_bytes_sent_{0};
    std::atomic<uint64_t> stat_bytes_recv_{0};
    std::atomic<uint64_t> stat_errors_{0};

    // ─────────────────────────────────────────────────────────────────
    // サービスループ: spec §7.1
    // ─────────────────────────────────────────────────────────────────
    void serve_loop(const RequestHandler& handler) noexcept {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            try {
                // 50ms ポーリング: stop() で stop_flag_=true をセット後、最大 50ms で終了
                auto fr = detail::recv_frame(*platform_, 50);

                if (fr.flags & detail::FLAG_REQUEST) {
                    // RPC リクエスト: handler を呼んでレスポンスを返送
                    Message resp{};
                    try {
                        resp = handler(fr.message);
                    } catch (...) {
                        // ハンドラ例外: 空のレスポンスを返して継続
                        resp = Message{};
                    }
                    std::lock_guard lk{io_mutex_};
                    detail::send_frame(*platform_, resp,
                                       fr.message_id, detail::FLAG_RESPONSE);
                    stat_msgs_recv_.fetch_add(1, std::memory_order_relaxed);
                    stat_bytes_recv_.fetch_add(fr.message.size(), std::memory_order_relaxed);
                    stat_msgs_sent_.fetch_add(1, std::memory_order_relaxed);
                    stat_bytes_sent_.fetch_add(resp.size(), std::memory_order_relaxed);
                }
                // FLAG_REQUEST が立っていないフレームは通常 receive 用だが、
                // serve_requests 実行中は receive 直接呼び出し禁止のため破棄する。
                // （spec §7.1 コメント参照）

            } catch (const PipeException& pe) {
                if (pe.pipe_code() == PipeErrorCode::Timeout) continue;  // stop_flag_ 再確認
                // 接続断 / stop() による close で read が失敗 → ループ終了
                (void)pe;
                break;
            } catch (...) {
                break;
            }
        }
        is_serving_.store(false, std::memory_order_release);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RpcPipeServer 公開 API
// ─────────────────────────────────────────────────────────────────────────────

RpcPipeServer::RpcPipeServer(std::string pipe_name, std::size_t buffer_size)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size))
{}

RpcPipeServer::RpcPipeServer(RpcPipeServer&&) noexcept = default;
RpcPipeServer& RpcPipeServer::operator=(RpcPipeServer&&) noexcept = default;
RpcPipeServer::~RpcPipeServer() { if (impl_) impl_->close(); }

void RpcPipeServer::listen()                                          { impl_->listen(); }
void RpcPipeServer::accept(std::chrono::milliseconds timeout)         { impl_->accept(timeout); }
void RpcPipeServer::close() noexcept                                  { impl_->close(); }
void RpcPipeServer::stop() noexcept                                   { impl_->stop(); }
void RpcPipeServer::send(const Message& msg)                          { impl_->send(msg); }
Message RpcPipeServer::receive(std::chrono::milliseconds timeout)     { return impl_->receive(timeout); }
void RpcPipeServer::serve_requests(RequestHandler handler, bool bg)   { impl_->serve_requests(std::move(handler), bg); }
bool RpcPipeServer::is_listening()  const noexcept { return impl_->is_listening(); }
bool RpcPipeServer::is_connected()  const noexcept { return impl_->is_connected(); }
bool RpcPipeServer::is_serving()    const noexcept { return impl_->is_serving(); }
const std::string& RpcPipeServer::pipe_name() const noexcept { return impl_->pipe_name(); }

PipeStats RpcPipeServer::stats() const noexcept {
    return impl_->stats_snapshot();
}

void RpcPipeServer::reset_stats() noexcept {
    impl_->reset_stats();
}

} // namespace pipeutil
