// pipe_error.cpp — PipeErrorCode の error_category 実装と PipeException
#include "pipeutil/pipe_error.hpp"
#include <string>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// PipeErrorCategory — std::error_category 実装
// ──────────────────────────────────────────────────────────────────────────────
namespace {

class PipeErrorCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "pipeutil";
    }

    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<PipeErrorCode>(ev)) {
            case PipeErrorCode::Ok:              return "success";
            case PipeErrorCode::SystemError:     return "system error";
            case PipeErrorCode::AccessDenied:    return "access denied";
            case PipeErrorCode::NotFound:        return "pipe not found";
            case PipeErrorCode::AlreadyConnected:return "already connected";
            case PipeErrorCode::NotConnected:    return "not connected";
            case PipeErrorCode::ConnectionReset: return "connection reset by peer";
            case PipeErrorCode::Timeout:         return "operation timed out";
            case PipeErrorCode::BrokenPipe:      return "broken pipe";
            case PipeErrorCode::Overflow:        return "buffer overflow";
            case PipeErrorCode::InvalidMessage:  return "invalid message format";
            case PipeErrorCode::InvalidArgument: return "invalid argument";
            case PipeErrorCode::NotSupported:    return "operation not supported";
            default:                             return "unknown pipeutil error";
        }
    }
};

} // anonymous namespace

const std::error_category& pipe_error_category() noexcept {
    // Meyers シングルトン（スレッドセーフ）
    static const PipeErrorCategory instance;
    return instance;
}

std::error_code make_error_code(PipeErrorCode code) noexcept {
    return {static_cast<int>(code), pipe_error_category()};
}

// ──────────────────────────────────────────────────────────────────────────────
// PipeException 実装
// ──────────────────────────────────────────────────────────────────────────────

PipeException::PipeException(PipeErrorCode code, const std::string& what_arg)
    : std::system_error(make_error_code(code), what_arg)
    , pipe_code_(code)
{}

PipeException::PipeException(PipeErrorCode code,
                             int           os_error,
                             const std::string& what_arg)
    : std::system_error(os_error, std::system_category(), what_arg)
    , pipe_code_(code)
{}

} // namespace pipeutil
