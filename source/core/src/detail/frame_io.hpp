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

} // namespace pipeutil::detail
