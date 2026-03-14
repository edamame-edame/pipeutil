// test_timeout.cpp — Timeout に関する PipeErrorCode / PipeException 単体テスト
// 対象: source/core/include/pipeutil/pipe_error.hpp
// 仕様: spec/F007_error_timeout.md §3（タイムアウト契約）

#include <gtest/gtest.h>
#include "pipeutil/pipe_error.hpp"
#include <system_error>
#include <chrono>

using namespace pipeutil;

// ─── Timeout PipeErrorCode の基本動作 ────────────────────────────────

TEST(TimeoutTest, ErrorCodeIsNonZero) {
    const auto ec = make_error_code(PipeErrorCode::Timeout);
    EXPECT_NE(ec.value(), 0);
}

TEST(TimeoutTest, ErrorCodeCategory) {
    const auto ec = make_error_code(PipeErrorCode::Timeout);
    EXPECT_EQ(&ec.category(), &pipe_error_category());
}

TEST(TimeoutTest, ErrorCodeValue_MatchesSpec) {
    // spec/03_pipe_api.md §4 に定義された enum 値: Timeout = 13
    EXPECT_EQ(static_cast<int>(PipeErrorCode::Timeout), 13);
}

// ─── Timeout PipeException の構築 ─────────────────────────────────────

TEST(TimeoutTest, ExceptionCarriesTimeoutCode) {
    const PipeException ex{PipeErrorCode::Timeout, "operation timed out"};
    EXPECT_EQ(ex.pipe_code(), PipeErrorCode::Timeout);
}

TEST(TimeoutTest, ExceptionWhatIsNonEmpty) {
    const PipeException ex{PipeErrorCode::Timeout, "deadline exceeded"};
    EXPECT_FALSE(std::string_view{ex.what()}.empty());
}

TEST(TimeoutTest, ExceptionIsCatchableAsSystemError) {
    // R-007: 公開 API のエラー契約は例外中心で std::system_error を継承している
    bool caught = false;
    try {
        throw PipeException{PipeErrorCode::Timeout, "timeout"};
    } catch (const std::system_error& e) {
        caught = true;
        EXPECT_EQ(e.code().value(), static_cast<int>(PipeErrorCode::Timeout));
    }
    EXPECT_TRUE(caught);
}

TEST(TimeoutTest, ExceptionIsCatchableAsStdException) {
    bool caught = false;
    try {
        throw PipeException{PipeErrorCode::Timeout, "timeout"};
    } catch (const std::exception&) {
        caught = true;
    }
    EXPECT_TRUE(caught);
}

// ─── Timeout の識別: 他コードと混同しないこと ─────────────────────────

TEST(TimeoutTest, DistinctFromBrokenPipe) {
    EXPECT_NE(PipeErrorCode::Timeout, PipeErrorCode::BrokenPipe);
    EXPECT_NE(make_error_code(PipeErrorCode::Timeout).value(),
              make_error_code(PipeErrorCode::BrokenPipe).value());
}

TEST(TimeoutTest, DistinctFromNotFound) {
    EXPECT_NE(PipeErrorCode::Timeout, PipeErrorCode::NotFound);
}

TEST(TimeoutTest, DistinctFromSystemError) {
    EXPECT_NE(PipeErrorCode::Timeout, PipeErrorCode::SystemError);
}

// ─── TooManyConnections は Timeout ではない (R-044 接続上限) ──────────

TEST(TimeoutTest, TooManyConnectionsIsDistinct) {
    EXPECT_NE(PipeErrorCode::Timeout, PipeErrorCode::TooManyConnections);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::TooManyConnections), 33);
}

// ─── 後方互換性: Timeout 値が変更されていないこと ─────────────────────

TEST(TimeoutTest, TimeoutValueStability) {
    // この値が変わるとプロトコル互換性が壊れる
    static_assert(static_cast<int>(PipeErrorCode::Timeout) == 13,
                  "PipeErrorCode::Timeout must be 13 for backwards compatibility");
    SUCCEED();
}
