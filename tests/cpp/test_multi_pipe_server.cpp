// test_multi_pipe_server.cpp — MultiPipeServer 統合テスト
// 対象: multi_pipe_server.hpp / multi_pipe_server.cpp
//
// 各テストは一意なパイプ名を使い、並列実行でも競合しないようにする。

#include <gtest/gtest.h>
#include "pipeutil/multi_pipe_server.hpp"
#include "pipeutil/pipe_client.hpp"
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/message.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace pipeutil;
using namespace std::chrono_literals;

// ─── ユーティリティ ───────────────────────────────────────────────────

namespace {

std::string unique_pipe(const char* suffix) {
    return std::string("mps_test_") + suffix;
}

} // namespace

// ─── T-001: 基本起動・停止 ────────────────────────────────────────────

TEST(MultiPipeServerTest, StartStop) {
    const auto name = unique_pipe("start_stop");
    MultiPipeServer srv{name, 2};

    // serve はブロッキングなので別スレッドで実行
    auto srv_fut = std::async(std::launch::async, [&] {
        srv.serve([](PipeServer /*conn*/) {});
    });

    // サーバーが起動するのを少し待つ
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(srv.is_serving());

    // 別スレッドから停止
    srv.stop();
    srv_fut.get();  // serve() が正常終了するのを待つ

    EXPECT_FALSE(srv.is_serving());
}

// ─── T-002: 単一クライアントとのラウンドトリップ ─────────────────────

TEST(MultiPipeServerTest, SingleClientEcho) {
    const auto name = unique_pipe("single_echo");
    MultiPipeServer srv{name, 2};

    std::atomic<bool> received{false};
    auto srv_fut = std::async(std::launch::async, [&] {
        srv.serve([&](PipeServer conn) {
            Message msg = conn.receive(3000ms);
            conn.send(msg);  // エコー返し
            received = true;
        });
    });

    std::this_thread::sleep_for(100ms);

    // クライアント接続
    PipeClient cli{name};
    cli.connect(3000ms);
    cli.send(Message{std::string_view{"ping"}});
    Message reply = cli.receive(3000ms);
    EXPECT_EQ(reply.as_string_view(), "ping");
    cli.close();

    // ハンドラが完了するのを待ってから停止
    std::this_thread::sleep_for(100ms);
    srv.stop();
    srv_fut.get();

    EXPECT_TRUE(received.load());
}

// ─── T-003: 複数クライアントの同時接続 ──────────────────────────────

TEST(MultiPipeServerTest, MultipleClientsParallel) {
    const auto name = unique_pipe("multi_parallel");
    constexpr int N = 4;
    MultiPipeServer srv{name, static_cast<std::size_t>(N)};

    std::atomic<int> counter{0};
    auto srv_fut = std::async(std::launch::async, [&] {
        srv.serve([&](PipeServer conn) {
            Message msg = conn.receive(3000ms);
            conn.send(msg);
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    });

    std::this_thread::sleep_for(100ms);

    // N クライアントを並行起動
    std::vector<std::future<void>> cli_futs;
    cli_futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        cli_futs.push_back(std::async(std::launch::async, [&, i] {
            PipeClient cli{name};
            cli.connect(3000ms);
            const std::string payload = "msg" + std::to_string(i);
            cli.send(Message{std::string_view{payload}});
            Message reply = cli.receive(3000ms);
            EXPECT_EQ(reply.as_string_view(), payload);
            cli.close();
        }));
    }

    // 全クライアント完了を待つ
    for (auto& f : cli_futs) f.get();

    // 全ハンドラ完了まで少し待ってから停止
    for (int i = 0; i < 50 && counter.load() < N; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    srv.stop();
    srv_fut.get();

    EXPECT_EQ(counter.load(), N);
}

// ─── T-004: max_connections スロット制限 ─────────────────────────────

TEST(MultiPipeServerTest, SlotLimitHonored) {
    const auto name = unique_pipe("slot_limit");
    constexpr std::size_t MAX = 2;
    MultiPipeServer srv{name, MAX};

    std::atomic<std::size_t> peak_active{0};
    std::atomic<int>         done_count{0};

    auto srv_fut = std::async(std::launch::async, [&] {
        srv.serve([&](PipeServer conn) {
            // ピーク同時接続数を記録
            const auto cur = srv.active_connections();
            std::size_t prev = peak_active.load();
            while (cur > prev &&
                   !peak_active.compare_exchange_weak(prev, cur)) {}

            // 接続をしばらく保持してスロットを占有する
            conn.receive(500ms);
            done_count.fetch_add(1, std::memory_order_relaxed);
        });
    });

    std::this_thread::sleep_for(100ms);

    // MAX + 1 クライアントを順番に接続（各クライアントはメッセージを送ってから切断）
    for (std::size_t i = 0; i < MAX + 1; ++i) {
        PipeClient cli{name};
        cli.connect(3000ms);
        cli.send(Message{std::string_view{"x"}});
        cli.close();
        std::this_thread::sleep_for(50ms);
    }

    // 全ハンドラ完了を待ってから停止
    for (int i = 0; i < 100 && done_count.load() < static_cast<int>(MAX + 1); ++i) {
        std::this_thread::sleep_for(20ms);
    }
    srv.stop();
    srv_fut.get();

    EXPECT_LE(peak_active.load(), MAX);
    EXPECT_EQ(done_count.load(), static_cast<int>(MAX + 1));
}

// ─── T-005: stop() 後に再び serve() が呼べること ─────────────────────

TEST(MultiPipeServerTest, ReserveAfterStop) {
    const auto name = unique_pipe("reserve");
    MultiPipeServer srv{name, 2};

    // 1回目の serve/stop
    {
        auto fut = std::async(std::launch::async, [&] {
            srv.serve([](PipeServer /*conn*/) {});
        });
        std::this_thread::sleep_for(100ms);
        srv.stop();
        fut.get();
    }

    EXPECT_FALSE(srv.is_serving());

    // 2回目の serve/stop
    {
        auto fut = std::async(std::launch::async, [&] {
            srv.serve([](PipeServer /*conn*/) {});
        });
        std::this_thread::sleep_for(100ms);
        srv.stop();
        fut.get();
    }

    EXPECT_FALSE(srv.is_serving());
}
