// rpc_pipe_client.cpp — RpcPipeClient pimpl 実装
// 仕様: spec/F002_rpc_message_id.md §4, §6
#include "pipeutil/rpc_pipe_client.hpp"
#include "pipeutil/pipe_error.hpp"
#include "detail/frame_io.hpp"
#include "detail/platform_factory.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace pipeutil {

// ─────────────────────────────────────────────────────────────────────────────
// RpcPipeClient::Impl
// ─────────────────────────────────────────────────────────────────────────────
class RpcPipeClient::Impl {
public:
    Impl(std::string name, std::size_t buf_size)
        : pipe_name_(std::move(name))
        , platform_(std::make_unique<detail::PlatformPipeImpl>(buf_size))
    {}

    ~Impl() { close(); }

    // ─── ライフサイクル ──────────────────────────────────────────────

    void connect(std::chrono::milliseconds timeout) {
        platform_->client_connect(pipe_name_, timeout.count());
        stop_flag_.store(false, std::memory_order_release);
        receiver_thread_ = std::thread([this] { receiver_loop(); });
    }

    void close() noexcept {
        stop_flag_.store(true, std::memory_order_release);
        // パイプを先に閉じて receiver_thread_ の recv_frame() をアンブロック
        platform_->client_close();
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    // ─── 通常 send (message_id = 0) ──────────────────────────────────

    void send(const Message& msg) {
        std::lock_guard lk{io_mutex_};
        detail::send_frame(*platform_, msg);
    }

    // ─── 通常 receive (message_id = 0, キューから取り出す) ───────────

    Message receive(std::chrono::milliseconds timeout) {
        std::unique_lock lk{recv_mutex_};
        auto pred = [this] { return !recv_queue_.empty() || recv_exc_; };

        if (timeout.count() == 0) {
            recv_cv_.wait(lk, pred);
        } else {
            if (!recv_cv_.wait_for(lk, timeout, pred)) {
                throw PipeException{PipeErrorCode::Timeout, "receive timed out"};
            }
        }

        // 接続断により通常受信も失敗する場合
        if (recv_queue_.empty() && recv_exc_) {
            std::rethrow_exception(recv_exc_);
        }

        Message msg = std::move(recv_queue_.front());
        recv_queue_.pop();
        return msg;
    }

    // ─── RPC send_request ────────────────────────────────────────────

    Message send_request(const Message& req, std::chrono::milliseconds timeout) {
        // 1. message_id 採番 (0 と 0xFFFFFFFF はスキップ)
        uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        if (id == detail::NO_MESSAGE_ID || id == detail::RESERVED_ID_MAX) {
            id = next_id_.fetch_add(1, std::memory_order_relaxed);
        }
        // wrap-around: pending_map_ に衝突する場合はスキップ
        {
            std::lock_guard lk{pending_mutex_};
            while (pending_map_.count(id)) {
                id = next_id_.fetch_add(1, std::memory_order_relaxed);
                if (id == detail::NO_MESSAGE_ID || id == detail::RESERVED_ID_MAX) {
                    id = next_id_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // 2. promise を pending_map_ に登録
        std::promise<Message> promise;
        auto future = promise.get_future();
        {
            std::lock_guard lk{pending_mutex_};
            pending_map_[id] = std::move(promise);
        }

        // 3. FLAG_REQUEST | message_id を付与してフレーム送信
        {
            std::lock_guard lk{io_mutex_};
            detail::send_frame(*platform_, req, id, detail::FLAG_REQUEST);
        }

        // 4. 応答を待機（タイムアウト付き）: spec §6.2 R-019
        if (timeout.count() == 0) {
            future.wait();   // 無限待機
        } else {
            if (future.wait_for(timeout) == std::future_status::timeout) {
                // タイムアウト: pending_map_ から削除
                std::lock_guard lk{pending_mutex_};
                pending_map_.erase(id);
                throw PipeException{PipeErrorCode::Timeout, "send_request timed out"};
            }
        }
        return future.get();  // PipeException の再送出もここで発生
    }

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool               is_connected() const noexcept { return platform_->is_connected(); }
    [[nodiscard]] const std::string& pipe_name()    const noexcept { return pipe_name_; }

private:
    std::string                            pipe_name_;
    std::unique_ptr<detail::IPlatformPipe> platform_;

    // 送信保護（send / send_request が同時に呼ばれる場合に I/O を直列化）
    std::mutex                             io_mutex_;

    // 背景受信スレッド
    std::thread                            receiver_thread_;
    std::atomic<bool>                      stop_flag_{false};

    // message_id 採番カウンタ（1 から開始）
    std::atomic<uint32_t>                  next_id_{1};

    // ─ 通常受信キュー (message_id == 0) ─────────────────────────────
    std::mutex                             recv_mutex_;
    std::condition_variable                recv_cv_;
    std::queue<Message>                    recv_queue_;
    std::exception_ptr                     recv_exc_;   // 接続断時に通常 receive へ伝播

    // ─ RPC 応答待ちテーブル ──────────────────────────────────────────
    std::mutex                             pending_mutex_;
    std::unordered_map<uint32_t, std::promise<Message>> pending_map_;

    // ─────────────────────────────────────────────────────────────────
    // 背景受信ループ
    // ─────────────────────────────────────────────────────────────────
    void receiver_loop() noexcept {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            try {
                // 無限待機: close() が platform_->client_close() を先に呼ぶことで
                // 接続断例外を発生させてループを抜ける設計
                auto fr = detail::recv_frame(*platform_, 0);
                dispatch(std::move(fr));
            } catch (const PipeException&) {
                // 接続断・close 呼び出しによる中断 → 全 pending に伝播して終了
                auto ep = std::current_exception();
                notify_all_pending(ep);
                notify_recv_exception(ep);
                break;
            } catch (...) {
                auto ep = std::current_exception();
                notify_all_pending(ep);
                notify_recv_exception(ep);
                break;
            }
        }
    }

    // フレームを受信キューか pending_map_ に振り分ける
    void dispatch(detail::FrameResult&& fr) {
        const uint32_t mid   = fr.message_id;
        const uint8_t  flags = fr.flags;

        if (mid != detail::NO_MESSAGE_ID && (flags & detail::FLAG_RESPONSE)) {
            // RPC 応答: pending_map_ から promise を探して set_value
            std::lock_guard lk{pending_mutex_};
            auto it = pending_map_.find(mid);
            if (it != pending_map_.end()) {
                it->second.set_value(std::move(fr.message));
                pending_map_.erase(it);
            }
            // 未知の ID（タイムアウト後の遅延応答等）は破棄
        } else {
            // 通常受信キューに積む
            {
                std::lock_guard lk{recv_mutex_};
                recv_queue_.push(std::move(fr.message));
            }
            recv_cv_.notify_one();
        }
    }

    // 全 pending promise に例外を伝播する
    void notify_all_pending(std::exception_ptr ep) noexcept {
        std::lock_guard lk{pending_mutex_};
        for (auto& [id, promise] : pending_map_) {
            try { promise.set_exception(ep); } catch (...) {}
        }
        pending_map_.clear();
    }

    // 通常受信の待機者に例外を伝播する
    void notify_recv_exception(std::exception_ptr ep) noexcept {
        {
            std::lock_guard lk{recv_mutex_};
            recv_exc_ = ep;
        }
        recv_cv_.notify_all();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RpcPipeClient 公開 API
// ─────────────────────────────────────────────────────────────────────────────

RpcPipeClient::RpcPipeClient(std::string pipe_name, std::size_t buffer_size)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size))
{}

RpcPipeClient::RpcPipeClient(RpcPipeClient&&) noexcept = default;
RpcPipeClient& RpcPipeClient::operator=(RpcPipeClient&&) noexcept = default;
RpcPipeClient::~RpcPipeClient() { if (impl_) impl_->close(); }

void RpcPipeClient::connect(std::chrono::milliseconds timeout) {
    impl_->connect(timeout);
}

void RpcPipeClient::close() noexcept {
    impl_->close();
}

void RpcPipeClient::send(const Message& msg) {
    impl_->send(msg);
}

Message RpcPipeClient::receive(std::chrono::milliseconds timeout) {
    return impl_->receive(timeout);
}

Message RpcPipeClient::send_request(const Message& request,
                                    std::chrono::milliseconds timeout) {
    return impl_->send_request(request, timeout);
}

bool RpcPipeClient::is_connected() const noexcept {
    return impl_->is_connected();
}

const std::string& RpcPipeClient::pipe_name() const noexcept {
    return impl_->pipe_name();
}

} // namespace pipeutil
