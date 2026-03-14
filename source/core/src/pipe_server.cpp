// pipe_server.cpp — PipeServer pimpl 実装
// フレーミング: spec/02_protocol.md
// プラットフォーム委譲: spec/03_platform.md
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"
#include "pipeutil/pipe_stats.hpp"
#include "detail/session_stats.hpp"     // MultiPipeServer 専用統計バッファ (F-006)
#include "detail/frame_io.hpp"          // send_frame / recv_frame 共通実装 (F-002)
#include "detail/platform_factory.hpp"

#include <atomic>
#include <mutex>

namespace pipeutil {

// ─── PipeServer::Impl ─────────────────────────────────────────────

class PipeServer::Impl {
public:
    Impl(std::string name, std::size_t buf_size,
         PipeAcl acl = PipeAcl::Default, std::string custom_sddl = "")
        : pipe_name_(std::move(name))
        , platform_(std::make_unique<detail::PlatformPipeImpl>(buf_size))
        , acl_(acl)
        , custom_sddl_(std::move(custom_sddl))
    {}

    /// 内部コンストラクタ: accept 済みの IPlatformPipe を受け取る（MultiPipeServer 専用）
    Impl(std::string name, std::size_t /*buf_size*/, std::unique_ptr<detail::IPlatformPipe> accepted)
        : pipe_name_(std::move(name))
        , platform_(std::move(accepted))
    {}

    /// 内部コンストラクタ: accept 済み + SessionStats 付き（MultiPipeServer 専用）
    Impl(std::string name, std::size_t /*buf_size*/,
         std::unique_ptr<detail::IPlatformPipe> accepted,
         std::shared_ptr<detail::SessionStats> session)
        : pipe_name_(std::move(name))
        , platform_(std::move(accepted))
        , session_stats_(std::move(session))
    {}

    void listen() {
        platform_->server_create(pipe_name_, acl_, custom_sddl_);
    }

    void accept(std::chrono::milliseconds timeout) {
        platform_->server_accept(timeout.count());
    }

    void close() noexcept {
        platform_->server_close();
    }

    void send(const Message& msg) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        try {
            detail::send_frame(*platform_, msg);
            stat_msgs_sent_.fetch_add(1, std::memory_order_relaxed);
            stat_bytes_sent_.fetch_add(msg.size(), std::memory_order_relaxed);
            if (session_stats_) {
                session_stats_->messages_sent.fetch_add(1, std::memory_order_relaxed);
                session_stats_->bytes_sent.fetch_add(msg.size(), std::memory_order_relaxed);
            }
        } catch (const PipeException&) {
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            if (session_stats_) {
                session_stats_->errors.fetch_add(1, std::memory_order_relaxed);
            }
            throw;
        }
    }

    Message receive(std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        try {
            auto fr = detail::recv_frame(*platform_, timeout.count());
            stat_msgs_recv_.fetch_add(1, std::memory_order_relaxed);
            stat_bytes_recv_.fetch_add(fr.message.size(), std::memory_order_relaxed);
            if (session_stats_) {
                session_stats_->messages_recv.fetch_add(1, std::memory_order_relaxed);
                session_stats_->bytes_recv.fetch_add(fr.message.size(), std::memory_order_relaxed);
            }
            return fr.message;
        } catch (const PipeException&) {
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            if (session_stats_) {
                session_stats_->errors.fetch_add(1, std::memory_order_relaxed);
            }
            throw;
        }
    }

    [[nodiscard]] bool is_listening() const noexcept {
        return platform_->is_server_listening();
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return platform_->is_connected();
    }

    [[nodiscard]] const std::string& pipe_name() const noexcept {
        return pipe_name_;
    }

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
    std::string                                    pipe_name_;
    std::unique_ptr<detail::IPlatformPipe>         platform_;
    std::mutex                                     io_mutex_;  // send/receive 保護
    PipeAcl                                        acl_;
    std::string                                    custom_sddl_;
    // ─── 統計カウンタ (F-006) ──────────────────────────────────────────────
    std::atomic<uint64_t> stat_msgs_sent_{0};
    std::atomic<uint64_t> stat_msgs_recv_{0};
    std::atomic<uint64_t> stat_bytes_sent_{0};
    std::atomic<uint64_t> stat_bytes_recv_{0};
    std::atomic<uint64_t> stat_errors_{0};
    // MultiPipeServer 統計合算用バッファ (F-006)；通常の PipeServer 使用時は nullptr
    std::shared_ptr<detail::SessionStats> session_stats_;
    // フレーム送受信ロジックは detail::send_frame / detail::recv_frame (frame_io.hpp) に集約
};

// ─── PipeServer 公開 API ──────────────────────────────────────────

PipeServer::PipeServer(std::string pipe_name, std::size_t buffer_size,
                       PipeAcl acl, std::string custom_sddl)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size,
                                    acl, std::move(custom_sddl)))
{}

/// MultiPipeServer 専用: accept 済みの IPlatformPipe を保持する PipeServer を構築する
PipeServer::PipeServer(FromAcceptedTag,
                       std::string pipe_name,
                       std::size_t buffer_size,
                       std::unique_ptr<detail::IPlatformPipe> accepted)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size, std::move(accepted)))
{}

/// MultiPipeServer 専用（SessionStats 付き）: 統計バッファを共有する PipeServer を構築する
PipeServer::PipeServer(FromAcceptedTag,
                       std::string pipe_name,
                       std::size_t buffer_size,
                       std::unique_ptr<detail::IPlatformPipe> accepted,
                       std::shared_ptr<detail::SessionStats> session)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size,
                                   std::move(accepted), std::move(session)))
{}

PipeServer::PipeServer(PipeServer&& other) noexcept = default;
PipeServer& PipeServer::operator=(PipeServer&& other) noexcept = default;
PipeServer::~PipeServer() {
    if (impl_) impl_->close();
}

void PipeServer::listen() {
    impl_->listen();
}

void PipeServer::accept(std::chrono::milliseconds timeout) {
    impl_->accept(timeout);
}

void PipeServer::close() noexcept {
    impl_->close();
}

void PipeServer::send(const Message& msg) {
    impl_->send(msg);
}

Message PipeServer::receive(std::chrono::milliseconds timeout) {
    return impl_->receive(timeout);
}

bool PipeServer::is_listening() const noexcept {
    return impl_->is_listening();
}

bool PipeServer::is_connected() const noexcept {
    return impl_->is_connected();
}

const std::string& PipeServer::pipe_name() const noexcept {
    return impl_->pipe_name();
}

PipeStats PipeServer::stats() const noexcept {
    return impl_->stats_snapshot();
}

void PipeServer::reset_stats() noexcept {
    impl_->reset_stats();
}

} // namespace pipeutil
