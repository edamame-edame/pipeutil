// test_security_acl.cpp — F-008 Windows セキュリティ記述子 ACL C++ ユニットテスト
// 仕様: spec/F008_security_acl.md §6.1
//
// TC1  DefaultAclServerListen        — PipeAcl::Default でリッスン成功
// TC2  DefaultAclRoundTrip           — Default ACL でクライアント接続・送受信成功
// TC3  LocalSystemAclServerListen    — PipeAcl::LocalSystem でリッスン成功
// TC4  EveryoneAclServerListen       — PipeAcl::Everyone でリッスン成功
// TC5  LocalSystemAclRoundTrip       — LocalSystem ACL でクライアント接続・送受信成功
// TC6  EveryoneAclRoundTrip          — Everyone ACL でクライアント接続・送受信成功
// TC7  CustomAclEmptySddlThrows      — Windows: 空 SDDL 文字列で InvalidArgument 例外
// TC8  CustomAclInvalidSddlThrows    — Windows: 不正 SDDL 文字列で AccessDenied 例外
// TC9  CustomAclLinuxThrows          — Linux: PipeAcl::Custom で InvalidArgument 例外
// TC10 MultiPipeServerDefaultAcl     — MultiPipeServer + Default ACL でサービス開始・停止
// TC11 MultiPipeServerEveryoneAcl    — MultiPipeServer + Everyone ACL でサービス開始・停止
// TC12 BackwardCompatDefaultCtor     — PipeAcl::Default がデフォルト引数（後方互換確認）

#include <gtest/gtest.h>
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_client.hpp"
#include "pipeutil/multi_pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/pipe_acl.hpp"

#include <chrono>
#include <future>
#include <string>
#include <thread>

using namespace pipeutil;
using namespace std::chrono_literals;

// ─── テスト用ユーティリティ ───────────────────────────────────────────

namespace {

std::string unique_pipe(const char* suffix) {
    return std::string("acl_test_") + suffix;
}

/// サーバーを非同期で listen/accept し fn を実行する
template <typename Fn>
std::future<void> run_server_async(const std::string& name, PipeAcl acl, Fn fn) {
    return std::async(std::launch::async, [name, acl, fn = std::move(fn)]() mutable {
        PipeServer srv{name, 65536, acl};
        srv.listen();
        srv.accept(5000ms);
        fn(srv);
        srv.close();
    });
}

} // namespace

// ─── TC1: Default ACL でリッスン成功 ─────────────────────────────────

TEST(SecurityAcl, DefaultAclServerListen) {
    PipeServer srv{unique_pipe("tc1"), 65536, PipeAcl::Default};
    ASSERT_NO_THROW(srv.listen());
    EXPECT_TRUE(srv.is_listening());
    srv.close();
}

// ─── TC2: Default ACL でラウンドトリップ ─────────────────────────────

TEST(SecurityAcl, DefaultAclRoundTrip) {
    const auto name = unique_pipe("tc2");
    auto server_fut = run_server_async(name, PipeAcl::Default, [](PipeServer& s) {
        auto msg = s.receive(5000ms);
        s.send(msg);  // エコー
    });

    // connect() は ERROR_FILE_NOT_FOUND を内部でリトライするため sleep 不要
    PipeClient client{name};
    ASSERT_NO_THROW(client.connect(3000ms));
    ASSERT_NO_THROW(client.send(Message{std::string{"ping"}}));
    auto reply = client.receive(3000ms);
    EXPECT_EQ(reply.as_string_view(), "ping");
    client.close();

    ASSERT_NO_THROW(server_fut.get());
}

// ─── TC3: LocalSystem ACL でリッスン成功 ─────────────────────────────

TEST(SecurityAcl, LocalSystemAclServerListen) {
    PipeServer srv{unique_pipe("tc3"), 65536, PipeAcl::LocalSystem};
    ASSERT_NO_THROW(srv.listen());
    EXPECT_TRUE(srv.is_listening());
    srv.close();
}

// ─── TC4: Everyone ACL でリッスン成功 ────────────────────────────────

TEST(SecurityAcl, EveryoneAclServerListen) {
    PipeServer srv{unique_pipe("tc4"), 65536, PipeAcl::Everyone};
    ASSERT_NO_THROW(srv.listen());
    EXPECT_TRUE(srv.is_listening());
    srv.close();
}

// ─── TC5: LocalSystem ACL でラウンドトリップ ─────────────────────────

TEST(SecurityAcl, LocalSystemAclRoundTrip) {
    const auto name = unique_pipe("tc5");
    auto server_fut = run_server_async(name, PipeAcl::LocalSystem, [](PipeServer& s) {
        auto msg = s.receive(5000ms);
        s.send(msg);
    });

    // connect() は ERROR_FILE_NOT_FOUND を内部でリトライするため sleep 不要
    PipeClient client{name};
    ASSERT_NO_THROW(client.connect(3000ms));
    ASSERT_NO_THROW(client.send(Message{std::string{"hello"}}));
    auto reply = client.receive(3000ms);
    EXPECT_EQ(reply.as_string_view(), "hello");
    client.close();

    ASSERT_NO_THROW(server_fut.get());
}

// ─── TC6: Everyone ACL でラウンドトリップ ────────────────────────────

TEST(SecurityAcl, EveryoneAclRoundTrip) {
    const auto name = unique_pipe("tc6");
    auto server_fut = run_server_async(name, PipeAcl::Everyone, [](PipeServer& s) {
        auto msg = s.receive(5000ms);
        s.send(msg);
    });

    // connect() は ERROR_FILE_NOT_FOUND を内部でリトライするため sleep 不要
    PipeClient client{name};
    ASSERT_NO_THROW(client.connect(3000ms));
    ASSERT_NO_THROW(client.send(Message{std::string{"world"}}));
    auto reply = client.receive(3000ms);
    EXPECT_EQ(reply.as_string_view(), "world");
    client.close();

    ASSERT_NO_THROW(server_fut.get());
}

// ─── TC7: Custom + 空 SDDL → InvalidArgument 例外（Windows のみ） ────

#ifdef _WIN32
TEST(SecurityAcl, CustomAclEmptySddlThrows) {
    PipeServer srv{unique_pipe("tc7"), 65536, PipeAcl::Custom, ""};
    EXPECT_THROW(
        { srv.listen(); },
        PipeException
    ) << "Empty custom_sddl must throw PipeException(InvalidArgument)";

    // 例外コードの確認
    try {
        srv.listen();
    } catch (const PipeException& e) {
        EXPECT_EQ(e.pipe_code(), PipeErrorCode::InvalidArgument);
    }
}

// ─── TC8: Custom + 不正 SDDL → AccessDenied 例外（Windows のみ） ────

TEST(SecurityAcl, CustomAclInvalidSddlThrows) {
    PipeServer srv{unique_pipe("tc8"), 65536, PipeAcl::Custom, "INVALID_SDDL!!!"};
    try {
        srv.listen();
        FAIL() << "Expected PipeException for invalid SDDL";
    } catch (const PipeException& e) {
        EXPECT_EQ(e.pipe_code(), PipeErrorCode::AccessDenied);
    }
}

// ─── TC9: Custom → Linux では InvalidArgument 例外（無効: このブロックは _WIN32 内） ──
// このテストは Linux 向け（下で定義）

#else  // !_WIN32

// ─── TC9: Linux では Custom → InvalidArgument 例外 ─────────────────

TEST(SecurityAcl, CustomAclLinuxThrows) {
    PipeServer srv{unique_pipe("tc9"), 65536, PipeAcl::Custom, "D:(A;;GA;;;WD)"};
    try {
        srv.listen();
        FAIL() << "Expected PipeException(InvalidArgument) on Linux for Custom ACL";
    } catch (const PipeException& e) {
        EXPECT_EQ(e.pipe_code(), PipeErrorCode::InvalidArgument);
    }
}

#endif  // _WIN32

// ─── TC10: MultiPipeServer + Default ACL でサービス開始・停止 ─────────

TEST(SecurityAcl, MultiPipeServerDefaultAcl) {
    MultiPipeServer srv{unique_pipe("tc10"), 4, 65536, PipeAcl::Default};
    auto stop_fut = std::async(std::launch::async, [&srv] {
        std::this_thread::sleep_for(100ms);
        srv.stop();
    });
    ASSERT_NO_THROW(srv.serve([](PipeServer /*conn*/) {}));
    stop_fut.get();
}

// ─── TC11: MultiPipeServer + Everyone ACL でサービス開始・停止 ────────

TEST(SecurityAcl, MultiPipeServerEveryoneAcl) {
    MultiPipeServer srv{unique_pipe("tc11"), 4, 65536, PipeAcl::Everyone};
    auto stop_fut = std::async(std::launch::async, [&srv] {
        std::this_thread::sleep_for(100ms);
        srv.stop();
    });
    ASSERT_NO_THROW(srv.serve([](PipeServer /*conn*/) {}));
    stop_fut.get();
}

// ─── TC12: デフォルト引数による後方互換確認 ──────────────────────────

TEST(SecurityAcl, BackwardCompatDefaultCtor) {
    // 既存の 2 引数コンストラクタが有効であることを確認（デフォルト = Default ACL）
    PipeServer srv{unique_pipe("tc12")};
    ASSERT_NO_THROW(srv.listen());
    EXPECT_TRUE(srv.is_listening());
    srv.close();

    MultiPipeServer msrv{unique_pipe("tc12m"), 2};
    auto stop_fut = std::async(std::launch::async, [&msrv] {
        std::this_thread::sleep_for(80ms);
        msrv.stop();
    });
    ASSERT_NO_THROW(msrv.serve([](PipeServer /*conn*/) {}));
    stop_fut.get();
}
