// test_stats.cpp — F-006 診断・メトリクス API C++ ユニットテスト
// 仕様: spec/F006_diagnostics_metrics.md §10.1
//
// TC1  stats_initial_zero           — 初期状態で全フィールドが 0
// TC2  stats_send_increments        — send() 後に messages_sent/bytes_sent 増加
// TC3  stats_receive_increments     — receive() 後に messages_received/bytes_received 増加
// TC4  stats_error_increments       — send() 失敗時に errors 増加
// TC5  stats_reset                  — reset_stats() 後に全フィールド 0
// TC6  stats_rpc_round_trip         — send_request() 後に rpc_calls/rtt_last_ns 正値
// TC7  stats_rpc_total_ns_accumulates — 複数回 send_request() で rtt_total_ns 累積
// TC8  stats_rpc_error_no_rtt       — タイムアウトで rpc_calls はインクリメントされない
// TC9  stats_thread_safe_send       — 並列 send() でも messages_sent が正確
// TC10 stats_plus_operator          — operator+ で 2 つの PipeStats を加算
// TC11 stats_pipe_client_avg_rtt_zero — PipeClient の avg_round_trip_ns() は常に 0
// TC12 stats_multi_server_aggregation — MultiPipeServer 全接続合算・reset_stats 後の罹染なし

#include <gtest/gtest.h>
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_client.hpp"
#include "pipeutil/rpc_pipe_client.hpp"
#include "pipeutil/rpc_pipe_server.hpp"
#include "pipeutil/multi_pipe_server.hpp"
#include "pipeutil/pipe_stats.hpp"
#include "pipeutil/pipe_error.hpp"

#include <chrono>
#include <future>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace pipeutil;
using namespace std::chrono_literals;

// ─── テスト用ユーティリティ ───────────────────────────────────────────

namespace {

std::string unique_pipe(const char* suffix) {
    return std::string("statstest_") + suffix;
}

// PipeServer を listen/accept して fn を実行する非同期ヘルパー
template <typename Fn>
std::future<void> run_server_async(const std::string& name, Fn fn) {
    return std::async(std::launch::async, [name, fn = std::move(fn)]() mutable {
        PipeServer srv{name};
        srv.listen();
        srv.accept(5000ms);
        fn(srv);
        srv.close();
    });
}

// RpcPipeServer を serve_requests する非同期ヘルパー
template <typename Handler>
std::future<void> run_rpc_server_async(const std::string& name, Handler h,
                                        RpcPipeServer** out_srv_ptr = nullptr) {
    return std::async(std::launch::async,
        [name, h = std::move(h), out_srv_ptr]() mutable {
            static thread_local RpcPipeServer srv{name};
            if (out_srv_ptr) *out_srv_ptr = &srv;
            srv.listen();
            srv.accept(5000ms);
            srv.serve_requests(std::move(h));
            srv.close();
        });
}

} // namespace

// ─── TC1: 初期状態で全フィールドが 0 ────────────────────────────────

TEST(StatsTest, stats_initial_zero) {
    PipeStats s;
    EXPECT_EQ(s.messages_sent,     0u);
    EXPECT_EQ(s.messages_received, 0u);
    EXPECT_EQ(s.bytes_sent,        0u);
    EXPECT_EQ(s.bytes_received,    0u);
    EXPECT_EQ(s.errors,            0u);
    EXPECT_EQ(s.rpc_calls,         0u);
    EXPECT_EQ(s.rtt_total_ns,      0u);
    EXPECT_EQ(s.rtt_last_ns,       0u);
    EXPECT_EQ(s.avg_round_trip_ns(), 0u);
}

// ─── TC2: send() 後に messages_sent/bytes_sent がインクリメント ──────

TEST(StatsTest, stats_send_increments) {
    const auto pipe = unique_pipe("send_inc");
    const std::string payload = "hello stats";

    auto srv_fut = run_server_async(pipe, [&](PipeServer& srv) {
        srv.receive(3000ms);  // クライアントから 1 メッセージ受け取るだけ
    });

    std::this_thread::sleep_for(30ms);

    PipeClient cli{pipe};
    cli.connect(3000ms);
    const auto s0 = cli.stats();
    EXPECT_EQ(s0.messages_sent, 0u);
    EXPECT_EQ(s0.bytes_sent,    0u);

    cli.send(Message{std::string_view{payload}});
    const auto s1 = cli.stats();
    EXPECT_EQ(s1.messages_sent, 1u);
    EXPECT_EQ(s1.bytes_sent,    static_cast<uint64_t>(payload.size()));

    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─── TC3: receive() 後に messages_received/bytes_received がインクリメント ─

TEST(StatsTest, stats_receive_increments) {
    const auto pipe = unique_pipe("recv_inc");
    const std::string payload = "from server";
    std::promise<void> client_done;

    auto srv_fut = std::async(std::launch::async, [&] {
        PipeServer srv{pipe};
        srv.listen();
        srv.accept(5000ms);
        srv.send(Message{std::string_view{payload}});
        client_done.get_future().wait();
        srv.close();
    });

    std::this_thread::sleep_for(30ms);

    PipeClient cli{pipe};
    cli.connect(3000ms);
    const auto s0 = cli.stats();
    EXPECT_EQ(s0.messages_received, 0u);
    EXPECT_EQ(s0.bytes_received,    0u);

    cli.receive(3000ms);
    const auto s1 = cli.stats();
    EXPECT_EQ(s1.messages_received, 1u);
    EXPECT_EQ(s1.bytes_received,    static_cast<uint64_t>(payload.size()));

    client_done.set_value();
    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─── TC4: send() 失敗時に errors がインクリメント ───────────────────

TEST(StatsTest, stats_error_increments) {
    const auto pipe = unique_pipe("err_inc");

    PipeClient cli{pipe};
    // 接続なしで send() すると PipeException が発生する
    ASSERT_THROW(cli.send(Message{std::string_view{"x"}}), PipeException);
    const auto s = cli.stats();
    // errors はインクリメントされる（NotConnected を PipeException サブクラスとして処理）
    EXPECT_GE(s.errors, 1u);
    EXPECT_EQ(s.messages_sent, 0u);  // 成功送信はなし
}

// ─── TC5: reset_stats() 後に全フィールドが 0 ─────────────────────────

TEST(StatsTest, stats_reset) {
    const auto pipe = unique_pipe("reset");

    auto srv_fut = run_server_async(pipe, [](PipeServer& srv) {
        srv.receive(3000ms);
    });

    std::this_thread::sleep_for(30ms);

    PipeClient cli{pipe};
    cli.connect(3000ms);
    cli.send(Message{std::string_view{"reset me"}});

    {
        const auto s = cli.stats();
        EXPECT_GT(s.messages_sent, 0u);
    }

    cli.reset_stats();
    {
        const auto s = cli.stats();
        EXPECT_EQ(s.messages_sent,     0u);
        EXPECT_EQ(s.messages_received, 0u);
        EXPECT_EQ(s.bytes_sent,        0u);
        EXPECT_EQ(s.bytes_received,    0u);
        EXPECT_EQ(s.errors,            0u);
    }

    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─── TC6: send_request() 後に rpc_calls/rtt_last_ns/avg_round_trip_ns が正値 ─

TEST(StatsTest, stats_rpc_round_trip) {
    const auto pipe = unique_pipe("rpc_rtt");

    auto srv_fut = std::async(std::launch::async, [&] {
        RpcPipeServer srv{pipe};
        srv.listen();
        srv.accept(5000ms);
        srv.serve_requests([](const Message& req) {
            return req;  // エコー
        });
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    RpcPipeClient cli{pipe};
    cli.connect(3000ms);

    cli.send_request(Message{std::string_view{"ping"}}, 3000ms);

    const auto s = cli.stats();
    EXPECT_EQ(s.rpc_calls,  1u);
    EXPECT_GT(s.rtt_last_ns, 0u);
    EXPECT_EQ(s.avg_round_trip_ns(), s.rtt_total_ns);  // rpc_calls == 1 なので avg == total

    cli.close();
    srv_fut.get();
}

// ─── TC7: 複数回 send_request() で rtt_total_ns が累積 ──────────────

TEST(StatsTest, stats_rpc_total_ns_accumulates) {
    const auto pipe = unique_pipe("rpc_total");
    constexpr int N = 5;

    auto srv_fut = std::async(std::launch::async, [&] {
        RpcPipeServer srv{pipe};
        srv.listen();
        srv.accept(5000ms);
        srv.serve_requests([](const Message& req) { return req; });
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    RpcPipeClient cli{pipe};
    cli.connect(3000ms);

    for (int i = 0; i < N; ++i) {
        cli.send_request(Message{std::string_view{"x"}}, 3000ms);
    }

    const auto s = cli.stats();
    EXPECT_EQ(s.rpc_calls, static_cast<uint64_t>(N));
    EXPECT_GE(s.rtt_total_ns, s.rtt_last_ns);  // 累積 >= 最後の 1 回
    EXPECT_EQ(s.avg_round_trip_ns(), s.rtt_total_ns / N);

    cli.close();
    srv_fut.get();
}

// ─── TC8: タイムアウト時に rpc_calls はインクリメントされない ────────

TEST(StatsTest, stats_rpc_error_no_rtt) {
    const auto pipe = unique_pipe("rpc_timeout");

    // タイムアウトするように接続なし（または応答なし）の状態でリクエストを送る
    // ここでは接続前に send_request を呼び出し PipeException を引き起こす
    RpcPipeClient cli{pipe};
    ASSERT_THROW(
        cli.send_request(Message{std::string_view{"x"}}, 1ms),
        PipeException);

    const auto s = cli.stats();
    EXPECT_EQ(s.rpc_calls,   0u);  // 失敗時はカウントしない
    EXPECT_EQ(s.rtt_last_ns, 0u);
    EXPECT_GE(s.errors,      1u);
}

// ─── TC9: 並列 send() でも messages_sent が正確 ──────────────────────

TEST(StatsTest, stats_thread_safe_send) {
    const auto pipe = unique_pipe("parallel");
    constexpr int N = 20;  // スレッド数
    constexpr int M = 10;  // スレッドあたりの送信回数

    // サーバー: N*M メッセージを受け取って捨てる
    auto srv_fut = std::async(std::launch::async, [&] {
        PipeServer srv{pipe};
        srv.listen();
        srv.accept(5000ms);
        for (int i = 0; i < N * M; ++i) {
            srv.receive(5000ms);
        }
        srv.close();
    });

    std::this_thread::sleep_for(30ms);

    PipeClient cli{pipe};
    cli.connect(3000ms);

    // N スレッドから各 M 回送信
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&cli, M] {
            for (int j = 0; j < M; ++j) {
                cli.send(Message{std::string_view{"x"}});
            }
        });
    }
    for (auto& t : threads) t.join();

    const auto s = cli.stats();
    EXPECT_EQ(s.messages_sent, static_cast<uint64_t>(N * M));

    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─── TC10: operator+ で 2 つの PipeStats を加算 ──────────────────────

TEST(StatsTest, stats_plus_operator) {
    PipeStats a;
    a.messages_sent     = 10;
    a.messages_received = 5;
    a.bytes_sent        = 1024;
    a.bytes_received    = 512;
    a.errors            = 2;
    a.rpc_calls         = 3;
    a.rtt_total_ns      = 300000;
    a.rtt_last_ns       = 100000;

    PipeStats b;
    b.messages_sent     = 20;
    b.messages_received = 15;
    b.bytes_sent        = 2048;
    b.bytes_received    = 1024;
    b.errors            = 1;
    b.rpc_calls         = 7;
    b.rtt_total_ns      = 700000;
    b.rtt_last_ns       = 90000;  // rtt_last_ns は lhs 優先（加算されない）

    PipeStats c = a + b;
    EXPECT_EQ(c.messages_sent,     30u);
    EXPECT_EQ(c.messages_received, 20u);
    EXPECT_EQ(c.bytes_sent,        3072u);
    EXPECT_EQ(c.bytes_received,    1536u);
    EXPECT_EQ(c.errors,            3u);
    EXPECT_EQ(c.rpc_calls,         10u);
    EXPECT_EQ(c.rtt_total_ns,      1000000u);
    EXPECT_EQ(c.rtt_last_ns,       100000u);  // lhs の値を維持
}

// ─── TC11: PipeClient の avg_round_trip_ns() は常に 0 ───────────────

TEST(StatsTest, stats_pipe_client_avg_rtt_zero) {
    const auto pipe = unique_pipe("avg_rtt_zero");

    auto srv_fut = run_server_async(pipe, [](PipeServer& srv) {
        srv.receive(3000ms);
        srv.send(Message{std::string_view{"ok"}});
    });

    std::this_thread::sleep_for(30ms);

    PipeClient cli{pipe};
    cli.connect(3000ms);
    cli.send(Message{std::string_view{"hi"}});
    cli.receive(3000ms);

    const auto s = cli.stats();
    EXPECT_EQ(s.rpc_calls,         0u);
    EXPECT_EQ(s.rtt_total_ns,      0u);
    EXPECT_EQ(s.rtt_last_ns,       0u);
    EXPECT_EQ(s.avg_round_trip_ns(), 0u);

    cli.close();
    ASSERT_NO_THROW(srv_fut.get());
}

// ─── TC12: MultiPipeServer 全接続合算・reset_stats 後の汚染なし ───────

TEST(StatsTest, stats_multi_server_aggregation) {
    const auto pipe = unique_pipe("multi_agg");
    constexpr int N = 3;  // 同時接続数
    constexpr int MSGS_PER_SESSION = 4;

    MultiPipeServer srv{pipe, N};
    std::promise<void> srv_started;

    // サーバーを背景スレッドで serve
    auto srv_thread = std::thread([&] {
        srv.serve([&](PipeServer conn) {
            // ハンドラ: クライアントから MSGS_PER_SESSION 回受信する
            for (int i = 0; i < MSGS_PER_SESSION; ++i) {
                conn.receive(5000ms);
            }
        });
    });

    // サーバーが起動するまで少し待つ
    std::this_thread::sleep_for(100ms);

    // N クライアントを同時接続して各 MSGS_PER_SESSION 送信
    std::vector<std::thread> clients;
    clients.reserve(N);
    for (int i = 0; i < N; ++i) {
        clients.emplace_back([&pipe] {
            PipeClient cli{pipe};
            cli.connect(3000ms);
            for (int j = 0; j < MSGS_PER_SESSION; ++j) {
                cli.send(Message{std::string_view{"hello"}});
            }
            cli.close();
        });
    }
    for (auto& t : clients) t.join();

    // 全セッション完了後に stats を確認
    // 少し待ってからスナップショット取得（スレッド間の集計を待つ）
    std::this_thread::sleep_for(100ms);
    srv.stop();
    srv_thread.join();

    const auto s = srv.stats();
    // サーバー側: N * MSGS_PER_SESSION メッセージを受信
    EXPECT_EQ(s.messages_received, static_cast<uint64_t>(N * MSGS_PER_SESSION));
    EXPECT_GT(s.bytes_received, 0u);

    // reset_stats() 後に旧値が混入しないこと
    srv.reset_stats();
    const auto s2 = srv.stats();
    EXPECT_EQ(s2.messages_received, 0u);
    EXPECT_EQ(s2.bytes_received,    0u);
    EXPECT_EQ(s2.errors,            0u);
}
