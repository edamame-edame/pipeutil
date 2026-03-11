// pipe_client.cpp — PipeClient pimpl 実装
// PipeServer::Impl と同じフレーミングロジックを共有する。
#include "pipeutil/pipe_client.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"
#include "pipeutil/pipe_stats.hpp"
#include "detail/frame_io.hpp"
#include "detail/platform_factory.hpp"

#include <atomic>
#include <mutex>

namespace pipeutil {

// ─── PipeClient::Impl ─────────────────────────────────────────────

class PipeClient::Impl {
public:
    Impl(std::string name, std::size_t buf_size)
        : pipe_name_(std::move(name))
        , platform_(std::make_unique<detail::PlatformPipeImpl>(buf_size))
    {}

    void connect(std::chrono::milliseconds timeout) {
        platform_->client_connect(pipe_name_, timeout.count());
    }

    void close() noexcept {
        platform_->client_close();
    }

    void send(const Message& msg) {
        std::lock_guard<std::mutex> lk(io_mutex_);
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
        std::lock_guard<std::mutex> lk(io_mutex_);
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
    std::string                            pipe_name_;
    std::unique_ptr<detail::IPlatformPipe> platform_;
    std::mutex                             io_mutex_;
    // ─── 統計カウンタ (F-006) ──────────────────────────────────────────────
    std::atomic<uint64_t> stat_msgs_sent_{0};
    std::atomic<uint64_t> stat_msgs_recv_{0};
    std::atomic<uint64_t> stat_bytes_sent_{0};
    std::atomic<uint64_t> stat_bytes_recv_{0};
    std::atomic<uint64_t> stat_errors_{0};
    // フレーム送受信ロジックは detail::send_frame / detail::recv_frame (frame_io.hpp) に集約
};

// ─── PipeClient 公開 API ──────────────────────────────────────────

PipeClient::PipeClient(std::string pipe_name, std::size_t buffer_size)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size))
{}

PipeClient::PipeClient(PipeClient&& other) noexcept = default;
PipeClient& PipeClient::operator=(PipeClient&& other) noexcept = default;
PipeClient::~PipeClient() {
    if (impl_) impl_->close();
}

void PipeClient::connect(std::chrono::milliseconds timeout) {
    impl_->connect(timeout);
}

void PipeClient::close() noexcept {
    impl_->close();
}

void PipeClient::send(const Message& msg) {
    impl_->send(msg);
}

Message PipeClient::receive(std::chrono::milliseconds timeout) {
    return impl_->receive(timeout);
}

bool PipeClient::is_connected() const noexcept {
    return impl_->is_connected();
}

const std::string& PipeClient::pipe_name() const noexcept {
    return impl_->pipe_name();
}

PipeStats PipeClient::stats() const noexcept {
    return impl_->stats_snapshot();
}

void PipeClient::reset_stats() noexcept {
    impl_->reset_stats();
}

} // namespace pipeutil
