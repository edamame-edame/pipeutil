// multi_pipe_server.cpp — MultiPipeServer pimpl 実装
// 仕様: spec/F001_multi_pipe_server.md
#include "pipeutil/multi_pipe_server.hpp"
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "detail/platform_factory.hpp"
#include "detail/session_stats.hpp"     // F-006 統計集約用

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace pipeutil {

// ─── MultiPipeServer::Impl ───────────────────────────────────────────────────

class MultiPipeServer::Impl {
public:
    Impl(std::string name, std::size_t max_conn, std::size_t buf_size,
         PipeAcl acl = PipeAcl::Default, std::string custom_sddl = "")
        : pipe_name_(std::move(name))
        , buffer_size_(buf_size)
        , max_connections_(max_conn)
        , acl_(acl)
        , custom_sddl_(std::move(custom_sddl))
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
        platform_->server_create(pipe_name_, acl_, custom_sddl_);

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

    // 全接続の stats を合算したスナップショットを返す（spec §4.3）
    [[nodiscard]] PipeStats stats_all() const noexcept {
        std::lock_guard<std::mutex> lk{stats_mutex_};
        PipeStats total = accumulated_stats_;
        for (const auto& s : active_stats_) {
            total += s->snapshot();
        }
        return total;
    }

    // 全接続の stats を一斉リセットする（spec §4.3）
    void reset_stats_all() noexcept {
        std::lock_guard<std::mutex> lk{stats_mutex_};
        accumulated_stats_ = PipeStats{};
        for (auto& s : active_stats_) {
            s->reset();
        }
    }

private:
    // ─── 設定 ──────────────────────────────────────────────────────────
    std::string  pipe_name_;
    std::size_t  buffer_size_;
    std::size_t  max_connections_;    PipeAcl      acl_;
    std::string  custom_sddl_;
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

    // ─── 診断・メトリクス (F-006) ──────────────────────────────────────
    // stats_mutex_ は accumulated_stats_ と active_stats_ の両方を保護する。
    // SlotGuard::~SlotGuard()・stats()・reset_stats() が共にこの 1 本を使用する。
    mutable std::mutex                                    stats_mutex_;
    PipeStats                                             accumulated_stats_{};
    std::vector<std::shared_ptr<detail::SessionStats>>    active_stats_;

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
                // セッション単位の統計バッファ（PipeServer と多重書き込み共有）
                auto session = std::make_shared<detail::SessionStats>();
                {
                    std::lock_guard<std::mutex> lk{stats_mutex_};
                    active_stats_.push_back(session);
                }

                // RAII: デストラクタが「アクティブリストからの除去」「累積への加算」
                //        「sem_.release()」「active_count_ デクリメント」を保証
                struct SlotGuard {
                    Impl& self;
                    std::shared_ptr<detail::SessionStats> session;
                    ~SlotGuard() {
                        {
                            // stats_mutex_ 1 本で除去と加算を原子化し、
                            // reset_stats() との競合を排除する（spec §4.3）
                            std::lock_guard<std::mutex> lk{self.stats_mutex_};
                            auto& v = self.active_stats_;
                            v.erase(std::remove(v.begin(), v.end(), session), v.end());
                            self.accumulated_stats_ += session->snapshot();
                        }
                        self.sem_.release();
                        // カウントが 0 になったら stop() の待機を起床させる
                        if (self.active_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            std::lock_guard<std::mutex> lk{self.done_mutex_};
                            self.done_cv_.notify_all();
                        }
                    }
                } guard{*this, session};

                // accept 済みの IPlatformPipe を包む PipeServer を構築（listen/accept 不要）
                // session を渡し、send/receive カウントをセッションバッファに dual-write する
                PipeServer conn{PipeServer::FromAcceptedTag{},
                                pipe_name_, buffer_size_,
                                std::move(pipe_arg), std::move(session)};
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
                                 std::size_t buffer_size,
                                 PipeAcl acl,
                                 std::string custom_sddl)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), max_connections, buffer_size,
                                    acl, std::move(custom_sddl)))
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

// ─── 診断・メトリクス (F-006) ─────────────────────────────────────────────

PipeStats MultiPipeServer::stats() const noexcept {
    return impl_->stats_all();
}

void MultiPipeServer::reset_stats() noexcept {
    impl_->reset_stats_all();
}

} // namespace pipeutil
