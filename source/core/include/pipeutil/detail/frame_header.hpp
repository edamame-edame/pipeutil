// detail/frame_header.hpp — ワイヤーフォーマットのヘッダ構造体定義
// プロトコル仕様は spec/02_protocol.md を参照。
#pragma once

#include <cstdint>

namespace pipeutil::detail {

// ──────────────────────────────────────────────────────────────────────────────
// FrameHeader — 16 バイト固定長ヘッダ
//
//  Offset | Size | Field
//  -------+------+---------------------------------------------------
//    0    |  4   | magic[4]       {'P','I','P','E'}
//    4    |  1   | version        0x01
//    5    |  1   | flags          FLAG_* ビット
//    6    |  2   | reserved[2]    0x00 0x00（送信時は必ず 0 で埋める）
//    8    |  4   | payload_size   ペイロードバイト数（LE）
//   12    |  4   | checksum       CRC-32C（ペイロードのみ対象、LE）
//  -------+------+---------------------------------------------------
//   合計  | 16   |
// ──────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)  // パディングなし（明示的レイアウト）
struct FrameHeader {
    uint8_t  magic[4];        // {'P','I','P','E'}
    uint8_t  version;         // PROTOCOL_VERSION
    uint8_t  flags;           // FLAG_* ビット
    uint8_t  reserved[2];     // 将来拡張用。送信時は 0x00 で埋めること
    uint32_t payload_size;    // ペイロードサイズ（リトルエンディアン）
    uint32_t checksum;        // CRC-32C（ペイロードのみ、リトルエンディアン）
};
#pragma pack(pop)

// FrameHeader のサイズは常に 16 バイトでなければならない
static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be exactly 16 bytes");

// ─── マジック・バージョン定数 ─────────────────────────────────────────
inline constexpr uint8_t MAGIC[4]          = {0x50, 0x49, 0x50, 0x45};  // "PIPE"
inline constexpr uint8_t PROTOCOL_VERSION  = 0x01;

// ─── flags ビット定義 ─────────────────────────────────────────────────
inline constexpr uint8_t FLAG_COMPRESSED   = 0x01;  // ペイロード圧縮済み（将来拡張）
inline constexpr uint8_t FLAG_ACK          = 0x02;  // ACK メッセージ（将来拡張）
// ビット 2〜7 は予約済み。送受信時は 0 を設定すること。

// ─── 空メッセージのチェックサム固定値 ────────────────────────────────
inline constexpr uint32_t EMPTY_CHECKSUM   = 0x00000000u;

} // namespace pipeutil::detail
