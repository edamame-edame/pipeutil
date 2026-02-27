// pipe_client.cpp — PipeClient pimpl 実装
// PipeServer::Impl と同じフレーミングロジックを共有する。
#include "pipeutil/pipe_client.hpp"
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
        send_frame(msg);
    }

    Message receive(std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        return recv_frame(timeout.count());
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

    void send_frame(const Message& msg) {
        const auto payload = msg.payload();
        // payload が uint32_t の最大値を超えていないことを検査する (R-014)
        if (payload.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            throw PipeException{PipeErrorCode::Overflow,
                                "Payload size exceeds uint32_t maximum (4 GiB-1)"};
        }
        const auto psz = static_cast<uint32_t>(payload.size());
        const uint32_t crc = payload.empty()
                               ? 0u
                               : detail::compute_crc32c(payload);

        detail::FrameHeader hdr{};
        std::memcpy(hdr.magic, detail::MAGIC, 4);
        hdr.version      = detail::PROTOCOL_VERSION;
        hdr.flags        = 0x00;
        hdr.reserved[0]  = 0x00;
        hdr.reserved[1]  = 0x00;
        hdr.payload_size = detail::to_le32(psz);
        hdr.checksum     = detail::to_le32(crc);

        platform_->write_all(reinterpret_cast<const std::byte*>(&hdr),
                              sizeof(hdr));
        if (!payload.empty()) {
            platform_->write_all(payload.data(), payload.size());
        }
    }

    Message recv_frame(int64_t timeout_ms) {
        detail::FrameHeader hdr{};
        platform_->read_all(reinterpret_cast<std::byte*>(&hdr),
                            sizeof(hdr), timeout_ms);

        if (std::memcmp(hdr.magic, detail::MAGIC, 4) != 0) {
            throw PipeException{PipeErrorCode::InvalidMessage, "Invalid magic bytes"};
        }
        if (hdr.version != detail::PROTOCOL_VERSION) {
            throw PipeException{PipeErrorCode::InvalidMessage, "Unsupported protocol version"};
        }

        const uint32_t psz          = detail::from_le32(hdr.payload_size);
        const uint32_t expected_crc = detail::from_le32(hdr.checksum);

        if (psz == 0) return Message{};

        std::vector<std::byte> buf(psz);
        platform_->read_all(buf.data(), psz, timeout_ms);

        const uint32_t computed_crc = detail::compute_crc32c(
            std::span<const std::byte>{buf.data(), buf.size()});
        if (computed_crc != expected_crc) {
            throw PipeException{PipeErrorCode::InvalidMessage, "CRC-32C mismatch"};
        }

        return Message{std::span<const std::byte>{buf.data(), buf.size()}};
    }
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
