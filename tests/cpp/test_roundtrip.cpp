// test_roundtrip.cpp — PipeServer / PipeClient 統合テスト（スレッド間 IPC）
// 対象: pipe_server.hpp, pipe_client.hpp
//
// 各テストは一意なパイプ名を使い、並列実行しても競合しないようにする。

#include <gtest/gtest.h>
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_client.hpp"
#include "pipeutil/pipe_error.hpp"
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>

using namespace pipeutil;
using namespace std::chrono_literals;

// ─── テスト用ユーティリティ ───────────────────────────────────────────

namespace {

// テストごとに固有のパイプ名を生成（並列テストでの競合を防ぐ）
std::string unique_pipe(const char* suffix) {
    return std::string("pipeutil_test_") + suffix;
}

// サーバースレッドを非同期で起動するヘルパー
// on_server: PipeServer& を受け取るラムダ
template <typename Fn>
std::future<void> run_server_async(const std::string& name, Fn on_server) {
    return std::async(std::launch::async, [name, on_server = std::move(on_server)]() mutable {
        PipeServer srv{name};
        srv.listen();
        srv.accept(5000ms);
        on_server(srv);
        srv.close();
    });
}

} // namespace

// ─── 基本ラウンドトリップ ─────────────────────────────────────────────

TEST(RoundTripTest, ClientToServer_SingleMessage) {
    const auto pipe_name = unique_pipe("basic_c2s");

    auto srv_fut = run_server_async(pipe_name, [](PipeServer& srv) {
        Message msg = srv.receive(3000ms);
        EXPECT_EQ(msg.as_string_view(), "hello");
    });

    // connect() は ERROR_FILE_NOT_FOUND を内部でリトライするため sleep 不要
    PipeClient cli{pipe_name};
    cli.connect(3000ms);
    cli.send(Message{std::string_view{"hello"}});
    cli.close();

    // サーバースレッドの例外を伝播させる
    ASSERT_NO_THROW(srv_fut.get());
}

TEST(RoundTripTest, ServerToClient_SingleMessage) {
    const auto pipe_name = unique_pipe("basic_s2c");
    // Windows では DisconnectNamedPipe がバッファを破棄するため、
    // クライアントが受信し終わるまでサーバーを生存させる必要がある。
    std::promise<void> client_done;

    auto srv_fut = std::async(std::launch::async, [&pipe_name, &client_done]() {
        PipeServer srv{pipe_name};
        srv.listen();
        srv.accept(5000ms);
        srv.send(Message{std::string_view{"world"}});
        // クライアントが受信し close() するまで待機してからサーバーを閉じる
        client_done.get_future().wait();
        srv.close();
    });

    PipeClient cli{pipe_name};
    cli.connect(3000ms);
    Message msg = cli.receive(3000ms);
    EXPECT_EQ(msg.as_string_view(), "world");
    cli.close();
    client_done.set_value();  // サーバーに「受信完了」を通知

    ASSERT_NO_THROW(srv_fut.get());
}

// ─── 双方向ラウンドトリップ ───────────────────────────────────────────

TEST(RoundTripTest, Bidirectional_EchoServer) {
    const auto pipe_name = unique_pipe("echo");
    std::promise<void> client_done;

    // エコーサーバー: 受信したものをそのまま送り返す
    auto srv_fut = std::async(std::launch::async, [&pipe_name, &client_done]() {
        PipeServer srv{pipe_name};
        srv.listen();
        srv.accept(5000ms);
        Message req = srv.receive(3000ms);
        srv.send(req);
        // クライアントが受信し close() するまで待機
        client_done.get_future().wait();
        srv.close();
    });

    PipeClient cli{pipe_name};
    cli.connect(3000ms);
    cli.send(Message{std::string_view{"echo_payload"}});
    Message resp = cli.receive(3000ms);
    EXPECT_EQ(resp.as_string_view(), "echo_payload");
    cli.close();
    client_done.set_value();  // サーバーに「受信完了」を通知

    ASSERT_NO_THROW(srv_fut.get());
}

// ─── 複数メッセージ ───────────────────────────────────────────────────

TEST(RoundTripTest, MultipleMessages) {
    const auto pipe_name = unique_pipe("multi");
    constexpr int kCount = 50;

    auto srv_fut = run_server_async(pipe_name, [kCount](PipeServer& srv) {
        for (int i = 0; i < kCount; ++i) {
            Message msg = srv.receive(3000ms);
            const std::string expected = "msg_" + std::to_string(i);
            EXPECT_EQ(msg.as_string_view(), expected);
        }
    });

    PipeClient cli{pipe_name};
    cli.connect(3000ms);
    for (int i = 0; i < kCount; ++i) {
        cli.send(Message{std::string_view{"msg_" + std::to_string(i)}});
    }
    cli.close();

    ASSERT_NO_THROW(srv_fut.get());
}

// ─── タイムアウト ─────────────────────────────────────────────────────

TEST(RoundTripTest, Receive_Timeout_ThrowsTimeoutException) {
    const auto pipe_name = unique_pipe("timeout_rx");

    // サーバーが何も送らないまま待機する
    auto srv_fut = std::async(std::launch::async, [pipe_name]() {
        PipeServer srv{pipe_name};
        srv.listen();
        srv.accept(5000ms);
        // 何も送らず 500 ms 待機してからクローズ
        std::this_thread::sleep_for(500ms);
        srv.close();
    });

    PipeClient cli{pipe_name};
    cli.connect(3000ms);

    // タイムアウトが PipeException(Timeout) を送出することを検証（R-015）
    try {
        cli.receive(100ms);
        FAIL() << "Expected PipeException(Timeout) was not thrown";
    } catch (const PipeException& e) {
        EXPECT_EQ(e.pipe_code(), PipeErrorCode::Timeout);
    }
    cli.close();
    srv_fut.get();
}

// ─── 大きなペイロード ─────────────────────────────────────────────────

TEST(RoundTripTest, LargePayload_1MiB) {
    const auto pipe_name = unique_pipe("large");
    constexpr std::size_t kPayloadSize = 1024 * 1024;  // 1 MiB

    std::vector<std::byte> payload(kPayloadSize, std::byte{0xCD});

    auto srv_fut = run_server_async(pipe_name, [&payload, kPayloadSize](PipeServer& srv) {
        Message msg = srv.receive(10000ms);
        ASSERT_EQ(msg.size(), kPayloadSize);
        EXPECT_EQ(msg.payload()[0],            std::byte{0xCD});
        EXPECT_EQ(msg.payload()[kPayloadSize-1],std::byte{0xCD});
    });

    PipeClient cli{pipe_name, kPayloadSize + 64};
    cli.connect(3000ms);
    cli.send(Message{std::span<const std::byte>(payload)});
    cli.close();

    ASSERT_NO_THROW(srv_fut.get());
}

// ─── 接続前の操作はエラーになること ──────────────────────────────────

TEST(RoundTripTest, SendWithoutConnect_ThrowsNotConnected) {
    PipeClient cli{unique_pipe("no_connect")};
    EXPECT_THROW(cli.send(Message{std::string_view{"x"}}), PipeException);
}

TEST(RoundTripTest, ReceiveWithoutConnect_ThrowsNotConnected) {
    PipeClient cli{unique_pipe("no_connect2")};
    EXPECT_THROW(cli.receive(100ms), PipeException);
}
