// pipe_server.cpp — PipeServer pimpl 実装
// フレーミング: spec/02_protocol.md
// プラットフォーム委譲: spec/03_platform.md
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"
#include "pipeutil/detail/frame_header.hpp"
#include "pipeutil/detail/endian.hpp"
#include "detail/crc32c.hpp"
#include "detail/platform_factory.hpp"

#include <mutex>
#include <cstring>
#include <limits>

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
        send_frame(msg);
    }

    Message receive(std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        return recv_frame(timeout.count());
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

    /// フレームを構築して送信する
    void send_frame(const Message& msg) {
        const auto payload = msg.payload();
        // payload が uint32_t の最大値を超えていないことを検査する (R-014)
        if (payload.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            throw PipeException{PipeErrorCode::Overflow,
                                "Payload size exceeds uint32_t maximum (4 GiB-1)"};
        }
        const auto psz = static_cast<uint32_t>(payload.size());

        // checksum: 空ペイロードは 0x00000000 固定（spec §8）
        const uint32_t crc = payload.empty()
                               ? 0u
                               : detail::compute_crc32c(payload);

        // FrameHeader 構築
        detail::FrameHeader hdr{};
        std::memcpy(hdr.magic, detail::MAGIC, 4);
        hdr.version      = detail::PROTOCOL_VERSION;
        hdr.flags        = 0x00;
        hdr.reserved[0]  = 0x00;
        hdr.reserved[1]  = 0x00;
        hdr.payload_size = detail::to_le32(psz);
        hdr.checksum     = detail::to_le32(crc);

        // ヘッダ送信
        platform_->write_all(reinterpret_cast<const std::byte*>(&hdr),
                              sizeof(hdr));
        // ペイロード送信（空なら何もしない）
        if (!payload.empty()) {
            platform_->write_all(payload.data(), payload.size());
        }
    }

    /// フレームを受信して Message を返す
    Message recv_frame(int64_t timeout_ms) {
        // ─ ヘッダ受信 ─────────────────────────────────────────────
        detail::FrameHeader hdr{};
        platform_->read_all(reinterpret_cast<std::byte*>(&hdr),
                            sizeof(hdr), timeout_ms);

        // マジック検証
        if (std::memcmp(hdr.magic, detail::MAGIC, 4) != 0) {
            throw PipeException{PipeErrorCode::InvalidMessage, "Invalid magic bytes"};
        }
        // バージョン検証
        if (hdr.version != detail::PROTOCOL_VERSION) {
            throw PipeException{PipeErrorCode::InvalidMessage, "Unsupported protocol version"};
        }

        const uint32_t psz = detail::from_le32(hdr.payload_size);
        const uint32_t expected_crc = detail::from_le32(hdr.checksum);

        // ─ ペイロード受信 ─────────────────────────────────────────
        if (psz == 0) {
            // 空メッセージ (checksum は 0 であるべきだが警告のみ)
            return Message{};
        }

        std::vector<std::byte> buf(psz);
        platform_->read_all(buf.data(), psz, timeout_ms);

        // CRC 検証
        const uint32_t computed_crc = detail::compute_crc32c(
            std::span<const std::byte>{buf.data(), buf.size()});
        if (computed_crc != expected_crc) {
            throw PipeException{PipeErrorCode::InvalidMessage, "CRC-32C mismatch"};
        }

        return Message{std::span<const std::byte>{buf.data(), buf.size()}};
    }
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
