// test_error.cpp — PipeErrorCode / PipeException 単体テスト
// 対象: source/core/include/pipeutil/pipe_error.hpp

#include <gtest/gtest.h>
#include "pipeutil/pipe_error.hpp"
#include <system_error>

using namespace pipeutil;

// ─── PipeErrorCode → error_code 変換 ─────────────────────────────────

TEST(PipeErrorTest, MakeErrorCode_OkIsZero) {
    const auto ec = make_error_code(PipeErrorCode::Ok);
    EXPECT_EQ(ec.value(), 0);
    EXPECT_EQ(&ec.category(), &pipe_error_category());
}

TEST(PipeErrorTest, MakeErrorCode_NonOkIsNonZero) {
    const auto ec = make_error_code(PipeErrorCode::Timeout);
    EXPECT_NE(ec.value(), 0);
}

TEST(PipeErrorTest, MakeErrorCode_CategoryConsistency) {
    // 全コードが同一カテゴリに属すること
    const std::error_category& cat = pipe_error_category();
    EXPECT_EQ(make_error_code(PipeErrorCode::SystemError).category(),  cat);
    EXPECT_EQ(make_error_code(PipeErrorCode::BrokenPipe).category(),   cat);
    EXPECT_EQ(make_error_code(PipeErrorCode::InvalidMessage).category(),cat);
}

// ─── PipeException 構築 ───────────────────────────────────────────────

TEST(PipeExceptionTest, ConstructWithCode_MessageContainsInfo) {
    const PipeException ex{PipeErrorCode::Timeout, "test timeout"};
    EXPECT_EQ(ex.pipe_code(), PipeErrorCode::Timeout);
    // what() は実装依存だが空でないことを確認
    EXPECT_NE(std::string_view{ex.what()}.empty(), true);
}

TEST(PipeExceptionTest, ConstructWithOsError) {
    const PipeException ex{PipeErrorCode::SystemError, 5 /*ERROR_ACCESS_DENIED相当*/, "os err"};
    EXPECT_EQ(ex.pipe_code(), PipeErrorCode::SystemError);
}

TEST(PipeExceptionTest, IsSystemError) {
    // PipeException は std::system_error を継承していること（R-007 対応）
    bool caught = false;
    try {
        throw PipeException{PipeErrorCode::BrokenPipe, "thrown"};
    } catch (const std::system_error&) {
        caught = true;
    }
    EXPECT_TRUE(caught) << "PipeException did not derive from std::system_error";
}

TEST(PipeExceptionTest, IsStdException) {
    // さらに std::exception を継承していること
    bool caught = false;
    try {
        throw PipeException{PipeErrorCode::NotConnected, "thrown"};
    } catch (const std::exception&) {
        caught = true;
    }
    EXPECT_TRUE(caught) << "PipeException did not derive from std::exception";
}

// ─── PipeErrorCode 各定数の値が仕様通りか ────────────────────────────

TEST(PipeErrorTest, ErrorCodeValues_MatchSpec) {
    // spec/03_pipe_api.md §4 に定義された enum 値と一致すること
    EXPECT_EQ(static_cast<int>(PipeErrorCode::Ok),               0);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::SystemError),      1);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::AccessDenied),     2);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::NotFound),         3);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::AlreadyConnected), 10);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::NotConnected),     11);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::ConnectionReset),  12);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::Timeout),          13);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::BrokenPipe),       20);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::Overflow),         21);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::InvalidMessage),   22);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::InvalidArgument),  30);
    EXPECT_EQ(static_cast<int>(PipeErrorCode::NotSupported),     31);
}
