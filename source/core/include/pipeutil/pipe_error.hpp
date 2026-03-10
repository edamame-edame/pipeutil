// pipe_error.hpp — PipeErrorCode 列挙体と PipeException クラス
#pragma once

#include "pipeutil_export.hpp"
#include <stdexcept>
#include <string>
#include <system_error>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// PipeErrorCode — パイプ操作で発生するエラー種別
// ──────────────────────────────────────────────────────────────────────────────
enum class PipeErrorCode : int {
    // 成功
    Ok               = 0,

    // OS / システムエラー
    SystemError      = 1,   // errno / GetLastError() 由来
    AccessDenied     = 2,
    NotFound         = 3,

    // 接続状態
    AlreadyConnected = 10,
    NotConnected     = 11,
    ConnectionReset  = 12,  // 相手側が切断した
    Timeout          = 13,

    // I/O
    BrokenPipe       = 20,
    Overflow         = 21,  // バッファサイズ超過
    InvalidMessage   = 22,  // フレーミングエラー（magic/version/CRC 不一致）

    // その他
    InvalidArgument      = 30,
    NotSupported         = 31,
    Interrupted          = 32,  // accept 操作が stop_accept() によって中断された
    TooManyConnections   = 33,  // R-044: 非同期接続数上限 (64) 超過
};

// PipeErrorCode を std::error_category に変換するユーティリティ（内部実装用）
PIPEUTIL_API const std::error_category& pipe_error_category() noexcept;
PIPEUTIL_API std::error_code            make_error_code(PipeErrorCode code) noexcept;

// ──────────────────────────────────────────────────────────────────────────────
// PipeException — pipeutil が送出する例外型
// std::system_error を継承しているため、OS エラーコードも保持できる。
// ──────────────────────────────────────────────────────────────────────────────
class PIPEUTIL_API PipeException : public std::system_error {
public:
    /// PipeErrorCode のみで構築（OS エラーコードなし）
    explicit PipeException(PipeErrorCode code,
                           const std::string& what_arg = "");

    /// OS エラーコード付きで構築
    explicit PipeException(PipeErrorCode code,
                           int           os_error,
                           const std::string& what_arg = "");

    /// ライブラリ固有のエラーコードを返す
    PipeErrorCode pipe_code() const noexcept { return pipe_code_; }

private:
    PipeErrorCode pipe_code_;
};

} // namespace pipeutil
