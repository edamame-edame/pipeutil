// src/detail/frame_io.hpp — フレーム送受信の共通内部実装（PRIVATE ヘッダ）
// pipe_server.cpp / pipe_client.cpp / rpc_pipe_client.cpp / rpc_pipe_server.cpp が共有する。
// 仕様: spec/02_protocol.md, spec/F002_rpc_message_id.md §2
#pragma once

#include "pipeutil/detail/frame_header.hpp"
#include "pipeutil/detail/endian.hpp"
#include "pipeutil/detail/platform_pipe.hpp"
#include "pipeutil/message.hpp"
#include "pipeutil/pipe_error.hpp"
#include "detail/crc32c.hpp"   // PRIVATE: src/ が include path に入っている

#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace pipeutil::detail {

// ─────────────────────────────────────────────────────────────────────────────
// FrameResult — recv_frame の戻り値
// ─────────────────────────────────────────────────────────────────────────────
struct FrameResult {
    pipeutil::Message message;
    uint32_t          message_id = 0;   ///< 0 = ID なし (NO_MESSAGE_ID)
    uint8_t           flags      = 0;   ///< 受信したフラグビット
};

// ─────────────────────────────────────────────────────────────────────────────
// send_frame — フレームを構築して IPlatformPipe に書き込む
//
// @param pipe        書き込み先プラットフォームパイプ
// @param msg         送信メッセージ
// @param message_id  RPC メッセージ ID; 0 = 通常 send
// @param extra_flags FLAG_REQUEST / FLAG_RESPONSE などの追加フラグ
// ─────────────────────────────────────────────────────────────────────────────
inline void send_frame(IPlatformPipe&           pipe,
                       const pipeutil::Message& msg,
                       uint32_t                 message_id  = NO_MESSAGE_ID,
                       uint8_t                  extra_flags = 0)
{
    const auto payload = msg.payload();

    // R-014: payload が uint32_t の最大値を超えていないことを検査
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::Overflow,
                                      "Payload size exceeds uint32_t maximum (4 GiB-1)"};
    }
    const auto     psz = static_cast<uint32_t>(payload.size());
    const uint32_t crc = payload.empty() ? 0u : compute_crc32c(payload);

    FrameHeader hdr{};
    std::memcpy(hdr.magic, MAGIC, 4);
    hdr.version      = PROTOCOL_VERSION;
    hdr.flags        = extra_flags;
    hdr.reserved[0]  = 0x00;
    hdr.reserved[1]  = 0x00;
    hdr.payload_size = to_le32(psz);
    hdr.checksum     = to_le32(crc);
    hdr.message_id   = to_le32(message_id);

    pipe.write_all(reinterpret_cast<const std::byte*>(&hdr), sizeof(hdr));
    if (!payload.empty()) {
        pipe.write_all(payload.data(), payload.size());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_frame — IPlatformPipe からフレーム全体を読み取って FrameResult を返す
//
// @param pipe        読み取り元プラットフォームパイプ
// @param timeout_ms  タイムアウト（ms）; 0 = 無限待機
// ─────────────────────────────────────────────────────────────────────────────
inline FrameResult recv_frame(IPlatformPipe& pipe, int64_t timeout_ms)
{
    FrameHeader hdr{};
    pipe.read_all(reinterpret_cast<std::byte*>(&hdr), sizeof(hdr), timeout_ms);

    if (std::memcmp(hdr.magic, MAGIC, 4) != 0) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "Invalid magic bytes"};
    }
    if (hdr.version != PROTOCOL_VERSION) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "Unsupported protocol version"};
    }

    const uint32_t psz          = from_le32(hdr.payload_size);
    const uint32_t expected_crc = from_le32(hdr.checksum);
    const uint32_t mid          = from_le32(hdr.message_id);
    const uint8_t  flags        = hdr.flags;

    if (psz == 0) {
        return FrameResult{pipeutil::Message{}, mid, flags};
    }

    std::vector<std::byte> buf(psz);
    pipe.read_all(buf.data(), psz, timeout_ms);

    const uint32_t computed_crc = compute_crc32c(
        std::span<const std::byte>{buf.data(), buf.size()});
    if (computed_crc != expected_crc) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "CRC-32C mismatch"};
    }

    return FrameResult{
        pipeutil::Message{std::span<const std::byte>{buf.data(), buf.size()}},
        mid,
        flags
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// send_hello — HELLO フレームを IPlatformPipe に書き込む (A-001)
//
// @param pipe                   書き込み先
// @param advertised_capabilities 自身が対応する capability ビットの OR
// ─────────────────────────────────────────────────────────────────────────────
inline void send_hello(IPlatformPipe& pipe, uint32_t advertised_capabilities)
{
    HelloPayload hp{};
    hp.feature_bitmap = to_le32(advertised_capabilities);
    hp.reserved       = 0u;

    const uint32_t crc = compute_crc32c(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(&hp), sizeof(hp)});

    FrameHeader hdr{};
    std::memcpy(hdr.magic, MAGIC, 4);
    hdr.version      = PROTOCOL_VERSION;
    hdr.flags        = FLAG_HELLO;
    hdr.reserved[0]  = 0x00;
    hdr.reserved[1]  = 0x00;
    hdr.payload_size = to_le32(static_cast<uint32_t>(sizeof(HelloPayload)));
    hdr.checksum     = to_le32(crc);
    hdr.message_id   = to_le32(NO_MESSAGE_ID);

    pipe.write_all(reinterpret_cast<const std::byte*>(&hdr), sizeof(hdr));
    pipe.write_all(reinterpret_cast<const std::byte*>(&hp),  sizeof(hp));
}

// ─────────────────────────────────────────────────────────────────────────────
// decode_hello_bitmap — 受信済み FrameHeader に対応するペイロードを読み取り
//                       client/server の feature_bitmap を返す (A-001)
//
// 前提: FrameHeader (hdr) の読み取りは呼び出し元が完了済み、
//        hdr.flags & FLAG_HELLO が成立していること。
// ─────────────────────────────────────────────────────────────────────────────
inline uint32_t decode_hello_bitmap(IPlatformPipe& pipe,
                                    const FrameHeader& hdr,
                                    int64_t timeout_ms)
{
    const uint32_t psz = from_le32(hdr.payload_size);
    if (psz < sizeof(HelloPayload)) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "HELLO payload too small"};
    }
    std::vector<std::byte> buf(psz);
    pipe.read_all(buf.data(), psz, timeout_ms);

    const uint32_t computed_crc = compute_crc32c(
        std::span<const std::byte>{buf.data(), psz});
    if (computed_crc != from_le32(hdr.checksum)) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "HELLO CRC-32C mismatch"};
    }

    HelloPayload hp{};
    std::memcpy(&hp, buf.data(), sizeof(HelloPayload));
    return from_le32(hp.feature_bitmap);
}

// ─────────────────────────────────────────────────────────────────────────────
// send_frame_v1compat — v1.0.0 クライアント向けに 16B FrameHeaderV1 フレームを送信
//                       v1-compat モードのサーバーが使用する (A-001 §4.3)
// ─────────────────────────────────────────────────────────────────────────────
inline void send_frame_v1compat(IPlatformPipe& pipe, const pipeutil::Message& msg)
{
    const auto payload = msg.payload();

    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::Overflow,
                                      "Payload size exceeds uint32_t maximum (4 GiB-1)"};
    }
    const auto     psz = static_cast<uint32_t>(payload.size());
    const uint32_t crc = payload.empty() ? 0u : compute_crc32c(payload);

    FrameHeaderV1 hdr{};
    std::memcpy(hdr.magic, MAGIC, 4);
    hdr.version      = 0x01;
    hdr.flags        = 0u;
    hdr.reserved[0]  = 0x00;
    hdr.reserved[1]  = 0x00;
    hdr.payload_size = to_le32(psz);
    hdr.checksum     = to_le32(crc);

    pipe.write_all(reinterpret_cast<const std::byte*>(&hdr), sizeof(hdr));
    if (!payload.empty()) {
        pipe.write_all(payload.data(), payload.size());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_frame_v1compat — v1.0.0 クライアントから 16B FrameHeaderV1 フレームを受信
//                       v1-compat モードのサーバーが使用する (A-001 §4.3)
// ─────────────────────────────────────────────────────────────────────────────
inline FrameResult recv_frame_v1compat(IPlatformPipe& pipe, int64_t timeout_ms)
{
    FrameHeaderV1 hdr{};
    pipe.read_all(reinterpret_cast<std::byte*>(&hdr), sizeof(hdr), timeout_ms);

    if (std::memcmp(hdr.magic, MAGIC, 4) != 0) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "Invalid magic bytes (v1-compat)"};
    }
    if (hdr.version != 0x01) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "Expected v1.0.0 frame (version=0x01) in v1-compat mode"};
    }

    const uint32_t psz          = from_le32(hdr.payload_size);
    const uint32_t expected_crc = from_le32(hdr.checksum);

    if (psz == 0) {
        return FrameResult{pipeutil::Message{}, NO_MESSAGE_ID, 0};
    }

    std::vector<std::byte> buf(psz);
    pipe.read_all(buf.data(), psz, timeout_ms);

    const uint32_t computed_crc = compute_crc32c(
        std::span<const std::byte>{buf.data(), buf.size()});
    if (computed_crc != expected_crc) {
        throw pipeutil::PipeException{pipeutil::PipeErrorCode::InvalidMessage,
                                      "CRC-32C mismatch (v1-compat)"};
    }

    return FrameResult{
        pipeutil::Message{std::span<const std::byte>{buf.data(), buf.size()}},
        NO_MESSAGE_ID,  // v1.0.0 フレームに message_id フィールドなし
        hdr.flags
    };
}

} // namespace pipeutil::detail
