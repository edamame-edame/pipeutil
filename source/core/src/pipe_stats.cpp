// pipe_stats.cpp — PipeStats 演算子実装 (F-006)
// 仕様: spec/F006_diagnostics_metrics.md §3.2
#include "pipeutil/pipe_stats.hpp"

namespace pipeutil {

PipeStats& PipeStats::operator+=(const PipeStats& rhs) noexcept {
    messages_sent     += rhs.messages_sent;
    messages_received += rhs.messages_received;
    bytes_sent        += rhs.bytes_sent;
    bytes_received    += rhs.bytes_received;
    errors            += rhs.errors;
    rpc_calls         += rhs.rpc_calls;
    rtt_total_ns      += rhs.rtt_total_ns;
    // rtt_last_ns は合算の意味がないため lhs を維持する
    return *this;
}

PipeStats operator+(PipeStats lhs, const PipeStats& rhs) noexcept {
    lhs += rhs;
    return lhs;
}

} // namespace pipeutil
