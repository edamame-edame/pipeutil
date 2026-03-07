// test_rpc.cpp — RpcPipeClient / RpcPipeServer 統合テスト
// 仕様: spec/F002_rpc_message_id.md §10

#include <gtest/gtest.h>
#include "pipeutil/rpc_pipe_client.hpp"
#include "pipeutil/rpc_pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/detail/frame_header.hpp"
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace pipeutil;
using namespace std::chrono_literals;

// ─── ユーティリティ ──────────────────────────────────────────────────────────

namespace {

std::string unique_pipe(const char* suffix) {
    return std::string("pipeutil_rpc_test_") + suffix;
}

// サーバーを非同期で起動するヘルパー
// on_server: (RpcPipeServer&) → void ラムダ
template <typename Fn>
std::future<void> async_rpc_server(const std::string& name, Fn on_server,
                                   std::chrono::milliseconds accept_timeout = 5000ms) {
    return std::async(std::launch::async,
        [name, accept_timeout, fn = std::move(on_server)]() mutable {
            RpcPipeServer srv{name};
            srv.listen();
            srv.accept(accept_timeout);
            fn(srv);
            srv.close();
        });
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// TC-F002-01: sizeof(FrameHeader) == 20
// ─────────────────────────────────────────────────────────────────────────────
TEST(RpcFrameTest, FrameHeader_Size_Is20) {
    static_assert(sizeof(detail::FrameHeader) == 20,
                  "FrameHeader must be 20 bytes (F-002 spec requirement)");
    EXPECT_EQ(sizeof(detail::FrameHeader), 20u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TC-F002-02: 単発ラウンドトリップ
// ─────────────────────────────────────────────────────────────────────────────
TEST(RpcTest, SendRequest_SingleRoundTrip) {
    const auto pipe_name = unique_pipe("single_rt");

    auto srv_fut = async_rpc_server(pipe_name, [](RpcPipeServer& srv) {
        // serve_requests: フォアグラウンドで 1 リクエストを処理して終了
        int handled = 0;
        srv.serve_requests([&handled](const Message& req) -> Message {
            ++handled;
            std::string resp_str = "echo:" + std::string{req.as_string_view()};
            return Message{std::string_view{resp_str}};
        }, /*run_in_background=*/false);
        EXPECT_EQ(handled, 1);
    });

    // サーバーの listen を待つ
    std::this_thread::sleep_for(50ms);

    RpcPipeClient cli{pipe_name};
    cli.connect(3000ms);

    Message resp = cli.send_request(Message{std::string_view{"ping"}}, 3000ms);
    EXPECT_EQ(resp.as_string_view(), "echo:ping");

    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// TC-F002-03: 並行 send_request (3 スレッド) — 応答が混在しないこと
// ─────────────────────────────────────────────────────────────────────────────
TEST(RpcTest, SendRequest_MultipleParallel) {
    const auto pipe_name = unique_pipe("parallel");
    constexpr int N_THREADS = 3;
    constexpr int N_REQUESTS_PER_THREAD = 5;

    auto srv_fut = async_rpc_server(pipe_name, [](RpcPipeServer& srv) {
        // 背景スレッドで serve_requests
        srv.serve_requests([](const Message& req) -> Message {
            // 要求メッセージ "req:X:Y" → レスポンス "resp:X:Y"
            std::string s{req.as_string_view()};
            s.replace(0, 3, "resp");
            return Message{std::string_view{s}};
        }, /*run_in_background=*/true);

        // しばらく待ってから stop
        std::this_thread::sleep_for(3000ms);
        srv.stop();
    });

    std::this_thread::sleep_for(50ms);

    RpcPipeClient cli{pipe_name};
    cli.connect(3000ms);

    // 複数スレッドから並行して send_request
    std::vector<std::future<void>> futs;
    futs.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        futs.push_back(std::async(std::launch::async, [&cli, t]() {
            for (int i = 0; i < N_REQUESTS_PER_THREAD; ++i) {
                std::string req_s = "req:" + std::to_string(t) + ":" + std::to_string(i);
                std::string exp_s = "resp:" + std::to_string(t) + ":" + std::to_string(i);
                Message resp = cli.send_request(Message{std::string_view{req_s}}, 3000ms);
                EXPECT_EQ(resp.as_string_view(), exp_s)
                    << "thread=" << t << " i=" << i;
            }
        }));
    }
    for (auto& f : futs) { ASSERT_NO_THROW(f.get()); }

    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// TC-F002-04: タイムアウト → Timeout 例外
// ─────────────────────────────────────────────────────────────────────────────
TEST(RpcTest, SendRequest_Timeout_ThrowsTimeout) {
    const auto pipe_name = unique_pipe("timeout");

    auto srv_fut = async_rpc_server(pipe_name, [](RpcPipeServer& srv) {
        // リクエストを受け取っても応答しない（タイムアウトを誘発）
        std::this_thread::sleep_for(2000ms);
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    RpcPipeClient cli{pipe_name};
    cli.connect(3000ms);

    EXPECT_THROW({
        cli.send_request(Message{std::string_view{"no_reply"}}, 300ms);
    }, PipeException);

    cli.close();
    // サーバー側の close が先に来るので get() は無視
    try { srv_fut.get(); } catch (...) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// TC-F002-05: 通常 send()/receive() と send_request() が共存できること
// ─────────────────────────────────────────────────────────────────────────────
TEST(RpcTest, PlainSend_CoexistsWithRpc) {
    const auto pipe_name = unique_pipe("coexist");

    auto srv_fut = async_rpc_server(pipe_name, [](RpcPipeServer& srv) {
        // (a) 通常フレームを受け取って返送
        Message plain = srv.receive(3000ms);
        srv.send(Message{plain});  // エコーバック

        // (b) RPC: 背景スレッドでサービス
        srv.serve_requests([](const Message& /*req*/) -> Message {
            return Message{std::string_view{"rpc_ok"}};
        }, /*run_in_background=*/true);

        std::this_thread::sleep_for(2000ms);
        srv.stop();
    });

    std::this_thread::sleep_for(50ms);

    RpcPipeClient cli{pipe_name};
    cli.connect(3000ms);

    // 通常 send→receive
    cli.send(Message{std::string_view{"plain_msg"}});
    Message plain_resp = cli.receive(3000ms);
    EXPECT_EQ(plain_resp.as_string_view(), "plain_msg");

    // RPC
    Message rpc_resp = cli.send_request(Message{std::string_view{"rpc_req"}}, 3000ms);
    EXPECT_EQ(rpc_resp.as_string_view(), "rpc_ok");

    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// TC-F002-06: サーバー切断 → pending に ConnectionReset が届くこと
// ─────────────────────────────────────────────────────────────────────────────
TEST(RpcTest, ServerDisconnect_RejectsAllPending) {
    const auto pipe_name = unique_pipe("disconnect");

    auto srv_fut = async_rpc_server(pipe_name, [](RpcPipeServer& srv) {
        // 接続後 200ms で切断（応答しない）
        std::this_thread::sleep_for(200ms);
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    RpcPipeClient cli{pipe_name};
    cli.connect(3000ms);

    // send_request を投げて、サーバー切断で例外を受け取る
    EXPECT_THROW({
        cli.send_request(Message{std::string_view{"will_fail"}}, 5000ms);
    }, PipeException);

    cli.close();
    try { srv_fut.get(); } catch (...) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// TC-F002-07: PROTOCOL_VERSION は 0x02
// ─────────────────────────────────────────────────────────────────────────────
TEST(RpcFrameTest, ProtocolVersion_Is_0x02) {
    EXPECT_EQ(detail::PROTOCOL_VERSION, uint8_t{0x02});
}
