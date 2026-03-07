// pipe_server.cpp — PipeServer pimpl 実装
// フレーミング: spec/02_protocol.md
// プラットフォーム委譲: spec/03_platform.md
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"
#include "detail/frame_io.hpp"          // send_frame / recv_frame 共通実装 (F-002)
#include "detail/platform_factory.hpp"

#include <mutex>

namespace pipeutil {

// ─── PipeServer::Impl ─────────────────────────────────────────────

class PipeServer::Impl {
public:
    Impl(std::string name, std::size_t buf_size)
        : pipe_name_(std::move(name))
        , platform_(std::make_unique<detail::PlatformPipeImpl>(buf_size))
    {}

    /// 内部コンストラクタ: accept 済みの IPlatformPipe を受け取る（MultiPipeServer 専用）
    Impl(std::string name, std::size_t /*buf_size*/, std::unique_ptr<detail::IPlatformPipe> accepted)
        : pipe_name_(std::move(name))
        , platform_(std::move(accepted))
    {}

    void listen() {
        platform_->server_create(pipe_name_);
    }

    void accept(std::chrono::milliseconds timeout) {
        platform_->server_accept(timeout.count());
    }

    void close() noexcept {
        platform_->server_close();
    }

    void send(const Message& msg) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        detail::send_frame(*platform_, msg);
    }

    Message receive(std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        return detail::recv_frame(*platform_, timeout.count()).message;
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

private:
    std::string                                    pipe_name_;
    std::unique_ptr<detail::IPlatformPipe>         platform_;
    std::mutex                                     io_mutex_;  // send/receive 保護
    // フレーム送受信ロジックは detail::send_frame / detail::recv_frame (frame_io.hpp) に集約
};

// ─── PipeServer 公開 API ──────────────────────────────────────────

PipeServer::PipeServer(std::string pipe_name, std::size_t buffer_size)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size))
{}

/// MultiPipeServer 専用: accept 済みの IPlatformPipe を保持する PipeServer を構築する
PipeServer::PipeServer(FromAcceptedTag,
                       std::string pipe_name,
                       std::size_t buffer_size,
                       std::unique_ptr<detail::IPlatformPipe> accepted)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size, std::move(accepted)))
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

} // namespace pipeutil
