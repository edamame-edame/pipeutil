// multi_pipe_server.cpp — MultiPipeServer pimpl 実装
// 仕様: spec/F001_multi_pipe_server.md
#include "pipeutil/multi_pipe_server.hpp"
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "detail/platform_factory.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>

namespace pipeutil {

// ─── MultiPipeServer::Impl ───────────────────────────────────────────────────

class MultiPipeServer::Impl {
public:
    Impl(std::string name, std::size_t max_conn, std::size_t buf_size)
        : pipe_name_(std::move(name))
        , buffer_size_(buf_size)
        , max_connections_(max_conn)
        , sem_(static_cast<std::ptrdiff_t>(max_conn))  // スロット上限をセマフォで管理
    {
        assert(max_conn >= 1 && max_conn <= 64);
    }

    ~Impl() {
        // デストラクタ呼び出し前に stop() されていない場合の安全弁
        if (serving_.load()) stop();
    }

    void serve(std::function<void(PipeServer)> handler) {
        if (serving_.exchange(true)) {
            throw PipeException{PipeErrorCode::AlreadyConnected,
                                "MultiPipeServer is already serving"};
        }
        stop_flag_ = false;
        handler_   = std::move(handler);

        // プラットフォーム初期化とリスン開始
        platform_ = std::make_unique<detail::PlatformPipeImpl>(buffer_size_);
        platform_->server_create(pipe_name_);

        // acceptor スレッド起動（run_acceptor がループを担う）
        acceptor_thread_ = std::thread([this] { run_acceptor(); });

        // stop() が全完了を通知するまでブロック
        std::unique_lock<std::mutex> lk{done_mutex_};
        done_cv_.wait(lk, [this] { return !serving_.load(); });
    }

    void stop() noexcept {
        if (!serving_.load()) return;

        stop_flag_ = true;
        // acceptor の server_accept_and_fork() を割り込み解除
        if (platform_) platform_->stop_accept();
        // acceptor スレッドの終了を待つ
        if (acceptor_thread_.joinable()) acceptor_thread_.join();

        // 実行中の全ハンドラスレッドが完了するのを待つ
        {
            std::unique_lock<std::mutex> lk{done_mutex_};
            done_cv_.wait(lk, [this] {
                return active_count_.load(std::memory_order_acquire) == 0;
            });
        }

        platform_.reset();
        serving_ = false;
        // serve() を unblock する
        {
            std::lock_guard<std::mutex> lk{done_mutex_};
            done_cv_.notify_all();
        }
    }

    [[nodiscard]] bool is_serving() const noexcept {
        return serving_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t active_connections() const noexcept {
        return active_count_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::string& pipe_name() const noexcept {
        return pipe_name_;
    }

private:
    // ─── 設定 ──────────────────────────────────────────────────────────
    std::string  pipe_name_;
    std::size_t  buffer_size_;
    std::size_t  max_connections_;

    // ─── 状態 ──────────────────────────────────────────────────────────
    std::function<void(PipeServer)>         handler_;
    std::unique_ptr<detail::IPlatformPipe>  platform_;
    std::thread                             acceptor_thread_;

    std::atomic<bool>        serving_{false};
    std::atomic<bool>        stop_flag_{false};
    std::atomic<std::size_t> active_count_{0};

    std::counting_semaphore<64> sem_;   // 同時接続スロット数を max_connections で初期化
    std::mutex                  done_mutex_;
    std::condition_variable     done_cv_;

    // ─── acceptor ループ ───────────────────────────────────────────────

    void run_acceptor() {
        while (!stop_flag_) {
            std::unique_ptr<detail::IPlatformPipe> accepted;
            try {
                // 0 = 無限待機; stop_accept() が stop_event_/stop_fd_ を設定して Interrupted を発生させる
                accepted = platform_->server_accept_and_fork(0);
            } catch (const PipeException& e) {
                if (e.pipe_code() == PipeErrorCode::Timeout)    continue;   // 念のため
                if (e.pipe_code() == PipeErrorCode::Interrupted) break;      // 正常中断
                if (stop_flag_)                                  break;      // stop 後のエラー
                // 予期しない重大エラー → ループ終了
                break;
            }

            // スロット確保（上限を超える場合はブロック）
            sem_.acquire();
            active_count_.fetch_add(1, std::memory_order_relaxed);

            // ハンドラスレッドを detach；RAII SlotGuard で確実に sem_.release() する
            std::thread([this, pipe_arg = std::move(accepted)]() mutable {
                // RAII: デストラクタが sem_.release() と active_count_ デクリメントを保証
                struct SlotGuard {
                    Impl& self;
                    ~SlotGuard() {
                        self.sem_.release();
                        // カウントが 0 になったら stop() の待機を起床させる
                        if (self.active_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            std::lock_guard<std::mutex> lk{self.done_mutex_};
                            self.done_cv_.notify_all();
                        }
                    }
                } guard{*this};

                // accept 済みの IPlatformPipe を包む PipeServer を構築（listen/accept 不要）
                PipeServer conn{PipeServer::FromAcceptedTag{},
                                pipe_name_, buffer_size_, std::move(pipe_arg)};
                try {
                    handler_(std::move(conn));
                } catch (...) {
                    // ハンドラ内例外を握り潰してスレッドを正常終了させる
                }
            }).detach();
        }
    }
};

// ─── MultiPipeServer 公開 API ─────────────────────────────────────────────────

MultiPipeServer::MultiPipeServer(std::string pipe_name,
                                 std::size_t max_connections,
                                 std::size_t buffer_size)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), max_connections, buffer_size))
{}

MultiPipeServer::~MultiPipeServer() = default;

void MultiPipeServer::serve(std::function<void(PipeServer)> handler) {
    impl_->serve(std::move(handler));
}

void MultiPipeServer::stop() noexcept {
    impl_->stop();
}

bool MultiPipeServer::is_serving() const noexcept {
    return impl_->is_serving();
}

std::size_t MultiPipeServer::active_connections() const noexcept {
    return impl_->active_connections();
}

const std::string& MultiPipeServer::pipe_name() const noexcept {
    return impl_->pipe_name();
}

} // namespace pipeutil
