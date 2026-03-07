// pipe_client.cpp — PipeClient pimpl 実装
// PipeServer::Impl と同じフレーミングロジックを共有する。
#include "pipeutil/pipe_client.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"
#include "detail/frame_io.hpp"          // send_frame / recv_frame 共通実装 (F-002)
#include "detail/platform_factory.hpp"

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
        detail::send_frame(*platform_, msg);
    }

    Message receive(std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        return detail::recv_frame(*platform_, timeout.count()).message;
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return platform_->is_connected();
    }

    [[nodiscard]] const std::string& pipe_name() const noexcept {
        return pipe_name_;
    }

private:
    std::string                            pipe_name_;
    std::unique_ptr<detail::IPlatformPipe> platform_;
    std::mutex                             io_mutex_;
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

} // namespace pipeutil
