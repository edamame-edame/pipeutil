// test_error_mapping.cpp — PipeErrorCode マッピング境界の単体テスト
// 対象: source/core/include/pipeutil/pipe_error.hpp
// 仕様: spec/F007_error_timeout.md §4（エラーコード→例外型マッピング契約）

#include <gtest/gtest.h>
#include "pipeutil/pipe_error.hpp"
#include <system_error>
#include <string>

using namespace pipeutil;

// ─── make_error_code が返すカテゴリの一意性 ──────────────────────────

TEST(ErrorMappingTest, CategoryIsSingleton) {
    // pipe_error_category() は常に同一インスタンスを返すこと
    EXPECT_EQ(&pipe_error_category(), &pipe_error_category());
}

TEST(ErrorMappingTest, CategoryNameIsNonEmpty) {
    EXPECT_FALSE(std::string{pipe_error_category().name()}.empty());
}

// ─── 全列挙値の make_error_code が同一カテゴリに属すること ──────────

TEST(ErrorMappingTest, AllCodes_SameCategory) {
    const std::error_category& cat = pipe_error_category();
    const PipeErrorCode codes[] = {
        PipeErrorCode::Ok,
        PipeErrorCode::SystemError,
        PipeErrorCode::AccessDenied,
        PipeErrorCode::NotFound,
        PipeErrorCode::AlreadyConnected,
        PipeErrorCode::NotConnected,
        PipeErrorCode::ConnectionReset,
        PipeErrorCode::Timeout,
        PipeErrorCode::BrokenPipe,
        PipeErrorCode::Overflow,
        PipeErrorCode::InvalidMessage,
        PipeErrorCode::InvalidArgument,
        PipeErrorCode::TooManyConnections,
    };
    for (const auto code : codes) {
        EXPECT_EQ(make_error_code(code).category(), cat)
            << "PipeErrorCode value=" << static_cast<int>(code)
            << " does not belong to pipe_error_category()";
    }
}

// ─── Ok コードは値 0（std::error_code との慣例に準拠） ──────────────

TEST(ErrorMappingTest, Ok_IsZero) {
    EXPECT_EQ(make_error_code(PipeErrorCode::Ok).value(), 0);
}

TEST(ErrorMappingTest, Ok_DoesNotEvaluateToTrue) {
    const std::error_code ec = make_error_code(PipeErrorCode::Ok);
    EXPECT_FALSE(ec) << "Ok error_code must be falsy (value == 0)";
}

// ─── 非 Ok コードはすべて値 != 0 かつ bool true ──────────────────────

TEST(ErrorMappingTest, NonOk_AreNonZeroAndTruthy) {
    const PipeErrorCode non_ok[] = {
        PipeErrorCode::SystemError,
        PipeErrorCode::AccessDenied,
        PipeErrorCode::NotFound,
        PipeErrorCode::AlreadyConnected,
        PipeErrorCode::NotConnected,
        PipeErrorCode::ConnectionReset,
        PipeErrorCode::Timeout,
        PipeErrorCode::BrokenPipe,
        PipeErrorCode::Overflow,
        PipeErrorCode::InvalidMessage,
        PipeErrorCode::InvalidArgument,
        PipeErrorCode::TooManyConnections,
    };
    for (const auto code : non_ok) {
        const std::error_code ec = make_error_code(code);
        EXPECT_NE(ec.value(), 0)
            << "PipeErrorCode value=" << static_cast<int>(code) << " should be non-zero";
        EXPECT_TRUE(ec)
            << "PipeErrorCode value=" << static_cast<int>(code) << " should be truthy";
    }
}

// ─── 各コードの固定 enum 値（後方互換性） ────────────────────────────

TEST(ErrorMappingTest, EnumValues_MatchSpec) {
    // spec/03_pipe_api.md §4 の定義値と一致すること。変更は ABI 破壊。
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
    EXPECT_EQ(static_cast<int>(PipeErrorCode::TooManyConnections), 33);
}

// ─── PipeException は各 PipeErrorCode で正しく構築される ─────────────

TEST(ErrorMappingTest, PipeException_ConstructsForAllCodes) {
    const PipeErrorCode codes[] = {
        PipeErrorCode::SystemError,
        PipeErrorCode::AccessDenied,
        PipeErrorCode::NotFound,
        PipeErrorCode::Timeout,
        PipeErrorCode::BrokenPipe,
        PipeErrorCode::InvalidMessage,
        PipeErrorCode::TooManyConnections,
    };
    for (const auto code : codes) {
        try {
            throw PipeException{code, "mapping test"};
        } catch (const PipeException& ex) {
            EXPECT_EQ(ex.pipe_code(), code);
            EXPECT_FALSE(std::string_view{ex.what()}.empty());
        }
    }
}

// ─── 異なるコードは異なる error_code 値を持つ（一意性） ──────────────

TEST(ErrorMappingTest, TimeoutAndAccessDenied_AreDifferent) {
    EXPECT_NE(make_error_code(PipeErrorCode::Timeout).value(),
              make_error_code(PipeErrorCode::AccessDenied).value());
}

TEST(ErrorMappingTest, BrokenPipeAndConnectionReset_AreDifferent) {
    EXPECT_NE(make_error_code(PipeErrorCode::BrokenPipe).value(),
              make_error_code(PipeErrorCode::ConnectionReset).value());
}
