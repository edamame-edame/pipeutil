// message.hpp — フレーミング付き IPC メッセージの不変値型
#pragma once

#include "pipeutil_export.hpp"
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// Message — ペイロードを保持する不変値型
// フレームヘッダ（magic / version / checksum 等）は send/receive 時に
// 自動付与・除去されるため、利用者はペイロードのみ扱う。
// ──────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API Message {
public:
    using PayloadType = std::vector<std::byte>;

    /// デフォルト構築（空メッセージ）
    Message() = default;

    /// バイト列スパンから構築（コピー）
    explicit Message(std::span<const std::byte> data);

    /// 文字列ビューから構築（UTF-8 バイト列として扱う）
    explicit Message(std::string_view text);

    // コピー・ムーブは値型として自然に許可
    Message(const Message&)            = default;
    Message& operator=(const Message&) = default;
    Message(Message&&)                 = default;
    Message& operator=(Message&&)      = default;

    ~Message() = default;

    // ─── アクセサ ─────────────────────────────────────────────────────

    /// ペイロード全体をスパンで返す（ゼロコピー読み取り）
    [[nodiscard]] std::span<const std::byte> payload()       const noexcept;
    [[nodiscard]] std::size_t                size()          const noexcept;
    [[nodiscard]] bool                       empty()         const noexcept;

    /// バイト列を文字列ビューとして解釈（ヌル終端なし、UTF-8 前提）
    [[nodiscard]] std::string_view as_string_view() const noexcept;

private:
    PayloadType data_;
};

} // namespace pipeutil
