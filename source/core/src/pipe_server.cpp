// pipe_server.cpp — PipeServer pimpl 実装
// フレーミング: spec/02_protocol.md
// プラットフォーム委譲: spec/03_platform.md
// Capability Negotiation: spec/A001_capability_negotiation.md
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"
#include "pipeutil/pipe_stats.hpp"
#include "detail/session_stats.hpp"     // MultiPipeServer 専用統計バッファ (F-006)
#include "detail/frame_io.hpp"          // send_frame / recv_frame 共通実装 (F-002)
#include "detail/platform_factory.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace pipeutil {

// ─── PipeServer::Impl ─────────────────────────────────────────────

class PipeServer::Impl {
public:
    Impl(std::string name, std::size_t buf_size,
         PipeAcl acl = PipeAcl::Default, std::string custom_sddl = "",
         HelloConfig hello_config = HelloConfig{})
        : pipe_name_(std::move(name))
        , platform_(std::make_unique<detail::PlatformPipeImpl>(buf_size))
        , acl_(acl)
        , custom_sddl_(std::move(custom_sddl))
        , hello_config_(std::move(hello_config))
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

    // ─── HELLO ハンドシェイク（A-001） ─────────────────────────────────────
    void accept(std::chrono::milliseconds timeout) {
        platform_->server_accept(timeout.count());
        do_hello_handshake();
    }

    [[nodiscard]] NegotiatedCapabilities negotiated_capabilities() const noexcept {
        return negotiated_;
    }

    void close() noexcept {
        platform_->server_close();
    }

    void send(const Message& msg) {
        std::lock_guard<std::mutex> lk(io_mutex_);
        try {
            if (negotiated_.v1_compat) {
                detail::send_frame_v1compat(*platform_, msg);
            } else {
                detail::send_frame(*platform_, msg);
            }
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
            detail::FrameResult fr;
            if (buffered_frame_.has_value()) {
                // HELLO 処理中に先読みしたフレームを返す（v1-compat / no-HELLO 初回フレーム）
                fr = std::move(*buffered_frame_);
                buffered_frame_.reset();
            } else if (negotiated_.v1_compat) {
                fr = detail::recv_frame_v1compat(*platform_, timeout.count());
            } else {
                fr = detail::recv_frame(*platform_, timeout.count());
            }
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
    PipeAcl                                        acl_         = PipeAcl::Default;
    std::string                                    custom_sddl_;
    HelloConfig                                    hello_config_;
    NegotiatedCapabilities                         negotiated_;
    std::optional<detail::FrameResult>             buffered_frame_;  // HELLO 先読み時のバッファ
    // ─── 統計カウンタ (F-006) ──────────────────────────────────────────────
    std::atomic<uint64_t> stat_msgs_sent_{0};
    std::atomic<uint64_t> stat_msgs_recv_{0};
    std::atomic<uint64_t> stat_bytes_sent_{0};
    std::atomic<uint64_t> stat_bytes_recv_{0};
    std::atomic<uint64_t> stat_errors_{0};
    // MultiPipeServer 統計合算用バッファ (F-006)；通常の PipeServer 使用時は nullptr
    std::shared_ptr<detail::SessionStats> session_stats_;
    // フレーム送受信ロジックは detail::send_frame / detail::recv_frame (frame_io.hpp) に集約

    // ─── HELLO ハンドシェイク実装 (A-001 §8) ─────────────────────────────
    void do_hello_handshake() {
        if (hello_config_.mode == HelloMode::Skip) {
            negotiated_ = NegotiatedCapabilities{};
            return;
        }

        // 先頭 5 バイト（magic[4] + version[1]）を hello_timeout で読み取る
        std::byte buf5[5] = {};
        bool timed_out = false;

        try {
            platform_->read_all(buf5, 5, hello_config_.hello_timeout.count());
        } catch (const PipeException& e) {
            if (e.pipe_code() == PipeErrorCode::Timeout ||
                e.pipe_code() == PipeErrorCode::ConnectionReset) {
                timed_out = true;
            } else {
                throw;
            }
        }

        if (timed_out) {
            if (hello_config_.mode == HelloMode::Strict) {
                throw PipeException{PipeErrorCode::ConnectionRejected,
                                    "HELLO timeout: no header bytes received"};
            }
            // Compat: v1 フォールバック（v1_compat=false）
            negotiated_ = NegotiatedCapabilities{};
            return;
        }

        // マジック検証
        if (std::memcmp(buf5, detail::MAGIC, 4) != 0) {
            throw PipeException{PipeErrorCode::InvalidMessage,
                                "Invalid magic bytes during HELLO"};
        }

        const uint8_t ver = static_cast<uint8_t>(buf5[4]);

        if (ver == 0x01) {
            // ── v1.0.0 クライアント ────────────────────────────────────────
            if (hello_config_.mode == HelloMode::Strict) {
                throw PipeException{PipeErrorCode::ConnectionRejected,
                                    "v1.0.0 client rejected (HelloMode::Strict)"};
            }
            // Compat: 残り 11B を読んで FrameHeaderV1 を完成させる
            std::byte hdr_rest[11] = {};
            platform_->read_all(hdr_rest, 11, hello_config_.hello_timeout.count());

            detail::FrameHeaderV1 hdr_v1{};
            std::memcpy(&hdr_v1, buf5, 5);
            std::memcpy(reinterpret_cast<std::byte*>(&hdr_v1) + 5, hdr_rest, 11);

            const uint32_t psz = detail::from_le32(hdr_v1.payload_size);
            std::vector<std::byte> payload(psz);
            if (psz > 0) {
                platform_->read_all(payload.data(), psz, hello_config_.hello_timeout.count());
                const uint32_t computed_crc = detail::compute_crc32c(
                    std::span<const std::byte>{payload.data(), psz});
                if (computed_crc != detail::from_le32(hdr_v1.checksum)) {
                    throw PipeException{PipeErrorCode::InvalidMessage,
                                        "CRC-32C mismatch (v1-compat first frame)"};
                }
            }

            // 最初のフレームをバッファに保存（receive() で返す）
            buffered_frame_ = detail::FrameResult{
                pipeutil::Message{std::span<const std::byte>{payload.data(), psz}},
                detail::NO_MESSAGE_ID,
                hdr_v1.flags
            };
            negotiated_ = NegotiatedCapabilities{0u, true};  // v1-compat モード

        } else if (ver == 0x02) {
            // ── v1.1.0 クライアント ────────────────────────────────────────
            std::byte hdr_rest[15] = {};
            platform_->read_all(hdr_rest, 15, hello_config_.hello_timeout.count());

            detail::FrameHeader hdr{};
            std::memcpy(&hdr, buf5, 5);
            std::memcpy(reinterpret_cast<std::byte*>(&hdr) + 5, hdr_rest, 15);

            if (hdr.flags & detail::FLAG_HELLO) {
                // HELLO フレーム受信 → ペイロードを読み取り、応答を送信
                const uint32_t client_bitmap = detail::decode_hello_bitmap(
                    *platform_, hdr, hello_config_.hello_timeout.count());

                detail::send_hello(*platform_, hello_config_.advertised_capabilities);

                negotiated_.bitmap    = client_bitmap & hello_config_.advertised_capabilities;
                negotiated_.v1_compat = false;
            } else {
                // FLAG_HELLO なし（v1.1.0 クライアントが Skip モードなど）
                if (hello_config_.mode == HelloMode::Strict) {
                    throw PipeException{PipeErrorCode::ConnectionRejected,
                                        "HELLO flag not set (HelloMode::Strict)"};
                }
                // Compat: 受信済みフレームをバッファに保存してフォールバック
                const uint32_t psz = detail::from_le32(hdr.payload_size);
                std::vector<std::byte> payload(psz);
                if (psz > 0) {
                    platform_->read_all(payload.data(), psz, hello_config_.hello_timeout.count());
                    const uint32_t computed_crc = detail::compute_crc32c(
                        std::span<const std::byte>{payload.data(), psz});
                    if (computed_crc != detail::from_le32(hdr.checksum)) {
                        throw PipeException{PipeErrorCode::InvalidMessage,
                                            "CRC-32C mismatch (no-HELLO fallback first frame)"};
                    }
                }
                buffered_frame_ = detail::FrameResult{
                    pipeutil::Message{std::span<const std::byte>{payload.data(), psz}},
                    detail::from_le32(hdr.message_id),
                    hdr.flags
                };
                negotiated_ = NegotiatedCapabilities{};
            }
        } else {
            throw PipeException{PipeErrorCode::InvalidMessage,
                                "Unsupported protocol version during HELLO"};
        }
    }
};

// ─── PipeServer 公開 API ──────────────────────────────────────────

PipeServer::PipeServer(std::string pipe_name, std::size_t buffer_size,
                       PipeAcl acl, std::string custom_sddl,
                       HelloConfig hello_config)
    : impl_(std::make_unique<Impl>(std::move(pipe_name), buffer_size,
                                    acl, std::move(custom_sddl),
                                    std::move(hello_config)))
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
    if (on_hello_complete) {
        on_hello_complete(impl_->negotiated_capabilities());
    }
}

NegotiatedCapabilities PipeServer::negotiated_capabilities() const noexcept {
    return impl_->negotiated_capabilities();
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
