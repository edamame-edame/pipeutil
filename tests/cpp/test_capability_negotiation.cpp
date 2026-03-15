// test_capability_negotiation.cpp — A-001 Capability Negotiation C++ テスト
// 仕様: spec/A001_capability_negotiation.md, docs/design_change_spec_v1.1.0.md §11.1
//
// T-CN-001 HelloPayloadSize       — sizeof(HelloPayload) == 8
// T-CN-002 FlagHelloValue         — FLAG_HELLO == 0x80、他フラグとのビット重複なし
// T-CN-003 FrameHeaderV1Size      — sizeof(FrameHeaderV1) == 16
// T-CN-004 HelloFrameEndToEnd     — 双方 v1.1.0 で HELLO 交換が完了すること
// T-CN-005 NegotiationBothV110    — 双方 v1.1.0 → negotiated.is_legacy_v1() == true
// T-CN-006 NegotiationCompatV1Client  — v1.0.0 クライアント + Compat → v1-compat
// T-CN-007 NegotiationStrictRejectsV1Client — v1.0.0 クライアント + Strict → ConnectionRejected
// T-CN-008 NegotiationSkipMode    — 双方 Skip → 通常メッセージ正常
// T-CN-009 NegotiationCompatTimeout   — Compat + 50ms + Skip クライアント → v1 fallback
// T-CN-010 NegotiationStrictTimeout   — Strict + 50ms + Skip クライアント → ConnectionRejected
// T-CN-011 BitAndNegotiation      — server=0x0F, client=0x05 → negotiated=0x05
// T-CN-012 OnHelloCompleteCallback — on_hello_complete が accept() 後に呼ばれる
// T-CN-013 NormalMessageAfterHello — HELLO 後に通常メッセージ 10 往復
// T-CN-014 V1CompatNormalMessages  — v1-compat で通常メッセージ 10 往復

#include <gtest/gtest.h>
#include "pipeutil/pipe_server.hpp"
#include "pipeutil/pipe_client.hpp"
#include "pipeutil/pipe_error.hpp"
#include "pipeutil/pipe_acl.hpp"
#include "pipeutil/capability.hpp"
#include "pipeutil/detail/frame_header.hpp"
#include "pipeutil/detail/endian.hpp"

// OS ネイティブ API（v1.0.0 クライアント模倣用）
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <cerrno>
#endif

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

using namespace pipeutil;
using namespace pipeutil::detail;
using namespace std::chrono_literals;

// ─── テスト専用 CRC-32C (Castagnoli, polynomial 0x82F63B78) ─────────────────
static uint32_t test_crc32c(const void* data, size_t len) {
    // ランタイムでルックアップテーブルを初期化（static で 1 回のみ）
    struct Table {
        std::array<uint32_t, 256> t{};
        Table() {
            for (uint32_t n = 0; n < 256; ++n) {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
                t[n] = c;
            }
        }
    };
    static const Table tbl;
    uint32_t crc = 0xFFFFFFFFu;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        crc = tbl.t[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ─── v1.0.0 クライアント模倣用 OS ネイティブパイプクライアント ─────────────
#ifdef _WIN32
class RawLegacyClient {
    HANDLE h_ = INVALID_HANDLE_VALUE;
public:
    explicit RawLegacyClient(const std::string& pipe_name, int timeout_ms = 3000) {
        const std::string full = "\\\\.\\pipe\\pipeutil_" + pipe_name;
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        while (true) {
            h_ = CreateFileA(full.c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h_ != INVALID_HANDLE_VALUE) break;
            const DWORD err = GetLastError();
            if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND)
                throw std::runtime_error("CreateFileA failed: " + std::to_string(err));
            if (GetTickCount64() >= deadline)
                throw std::runtime_error("timeout connecting to named pipe");
            Sleep(10);
        }
    }
    ~RawLegacyClient() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

    void write_all(const void* data, size_t len) {
        DWORD written = 0;
        if (!WriteFile(h_, data, static_cast<DWORD>(len), &written, nullptr) ||
                written != static_cast<DWORD>(len))
            throw std::runtime_error("WriteFile failed");
    }
    void read_all(void* buf, size_t len) {
        DWORD total = 0;
        while (total < static_cast<DWORD>(len)) {
            DWORD rd = 0;
            if (!ReadFile(h_, static_cast<char*>(buf) + total,
                          static_cast<DWORD>(len) - total, &rd, nullptr))
                throw std::runtime_error("ReadFile failed");
            total += rd;
        }
    }
};
#else
// POSIX 版（Unix ドメインソケット）
class RawLegacyClient {
    int fd_ = -1;
public:
    explicit RawLegacyClient(const std::string& pipe_name, int timeout_ms = 3000) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string path = "/tmp/pipeutil_" + pipe_name;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("timeout connecting to socket");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~RawLegacyClient() { if (fd_ >= 0) ::close(fd_); }
    void write_all(const void* data, size_t len) {
        size_t total = 0;
        while (total < len) {
            const ssize_t n = ::write(fd_, static_cast<const char*>(data) + total,
                                      len - total);
            if (n <= 0) throw std::runtime_error("write() failed");
            total += static_cast<size_t>(n);
        }
    }
    void read_all(void* buf, size_t len) {
        size_t total = 0;
        while (total < len) {
            const ssize_t n = ::read(fd_, static_cast<char*>(buf) + total, len - total);
            if (n <= 0) throw std::runtime_error("read() failed");
            total += static_cast<size_t>(n);
        }
    }
};
#endif

// ─── テストユーティリティ ─────────────────────────────────────────────

namespace {

std::string unique_pipe(const char* suffix) {
    return std::string("captest_") + suffix;
}

// サーバースレッドを非同期で起動するヘルパー
template <typename Fn>
std::future<void> run_server_async(const std::string& name,
                                   HelloConfig cfg,
                                   Fn on_server)
{
    return std::async(std::launch::async,
        [name, cfg, on_server = std::move(on_server)]() mutable {
            PipeServer srv{name, 65536, PipeAcl::Default, "", cfg};
            srv.listen();
            srv.accept(5000ms);
            on_server(srv);
            srv.close();
        });
}

// v1.0.0 クライアント模倣: FrameHeaderV1 フレームを 1 つ送信する
static void send_v1_frame(RawLegacyClient& raw, std::string_view payload) {
    const auto psz = static_cast<uint32_t>(payload.size());
    const uint32_t crc = payload.empty()
        ? 0u : test_crc32c(payload.data(), payload.size());
    FrameHeaderV1 h{};
    std::memcpy(h.magic, MAGIC, 4);
    h.version      = 0x01;
    h.flags        = 0u;
    h.reserved[0]  = 0x00;
    h.reserved[1]  = 0x00;
    h.payload_size = to_le32(psz);
    h.checksum     = to_le32(crc);
    raw.write_all(&h, sizeof(h));
    if (!payload.empty())
        raw.write_all(payload.data(), payload.size());
}

// v1.0.0 クライアント模倣: FrameHeaderV1 レスポンスを 1 つ受信する
static std::string recv_v1_response(RawLegacyClient& raw) {
    FrameHeaderV1 h{};
    raw.read_all(&h, sizeof(h));
    const uint32_t psize = from_le32(h.payload_size);
    if (psize == 0) return {};
    std::string result(psize, '\0');
    raw.read_all(result.data(), psize);
    return result;
}

// --- v1.0.0 クライアント模倣ヘルパー ---
// 指定パイプへ接続後、FrameHeaderV1 フレーム 1 つを送り込む。
// 戻り値: サーバーが送り返したデータ（v1compat なら FrameHeaderV1 payload）
std::string legacy_connect_and_send(const std::string& pipe_name,
                                    std::string_view payload,
                                    bool read_response = false)
{
    RawLegacyClient raw{pipe_name, 3000};
    send_v1_frame(raw, payload);
    if (read_response)
        return recv_v1_response(raw);
    return {};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-001 HelloPayloadSize
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, HelloPayloadSize) {
    EXPECT_EQ(sizeof(HelloPayload), 8u);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-002 FlagHelloValue
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, FlagHelloValue) {
    EXPECT_EQ(FLAG_HELLO, 0x80u);
    // 既存フラグとのビット重複がないこと
    EXPECT_EQ(FLAG_HELLO & FLAG_COMPRESSED,  0u);
    EXPECT_EQ(FLAG_HELLO & FLAG_ACK,         0u);
    EXPECT_EQ(FLAG_HELLO & FLAG_REQUEST,     0u);
    EXPECT_EQ(FLAG_HELLO & FLAG_RESPONSE,    0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-003 FrameHeaderV1Size
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, FrameHeaderV1Size) {
    EXPECT_EQ(sizeof(FrameHeaderV1), 16u);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-004 HelloFrameEndToEnd — 双方 v1.1.0 で HELLO 交換が完了すること
// (send_hello / decode_hello_bitmap の間接検証)
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, HelloFrameEndToEnd) {
    const auto name = unique_pipe("hello_e2e");

    auto srv_fut = run_server_async(name, HelloConfig{HelloMode::Compat},
        [](PipeServer& srv) {
            // HELLO 完了後に negotiated_capabilities が読めること
            const auto caps = srv.negotiated_capabilities();
            EXPECT_FALSE(caps.is_v1_compat());
            srv.send(Message{std::string_view{"ok"}});
        });

    std::this_thread::sleep_for(50ms);

    PipeClient cli{name, 65536, HelloConfig{HelloMode::Compat}};
    cli.connect(3000ms);
    Message msg = cli.receive(3000ms);
    EXPECT_EQ(msg.as_string_view(), "ok");
    cli.close();
    srv_fut.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-005 NegotiationBothV110
// 双方 v1.1.0（bitmap=0）→ negotiated.is_legacy_v1() == true
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, NegotiationBothV110) {
    const auto name = unique_pipe("both_v110");

    NegotiatedCapabilities server_caps;
    auto srv_fut = run_server_async(name, HelloConfig{HelloMode::Compat},
        [&server_caps](PipeServer& srv) {
            server_caps = srv.negotiated_capabilities();
            srv.send(Message{std::string_view{"pong"}});
        });

    std::this_thread::sleep_for(50ms);

    PipeClient cli{name, 65536, HelloConfig{HelloMode::Compat}};
    cli.connect(3000ms);
    Message m = cli.receive(3000ms);
    EXPECT_EQ(m.as_string_view(), "pong");
    cli.close();
    srv_fut.get();

    // bitmap=0, v1_compat=false → is_legacy_v1()=true
    EXPECT_EQ(server_caps.bitmap, 0u);
    EXPECT_FALSE(server_caps.v1_compat);
    EXPECT_TRUE(server_caps.is_legacy_v1());
    EXPECT_FALSE(server_caps.is_v1_compat());
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-006 NegotiationCompatV1Client
// v1.0.0 クライアント（version=0x01 フレーム）+ サーバー Compat → v1-compat
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, NegotiationCompatV1Client) {
    const auto name = unique_pipe("compat_v1cli");

    NegotiatedCapabilities server_caps;
    auto srv_fut = std::async(std::launch::async, [&]() {
        PipeServer srv{name, 65536, PipeAcl::Default, "", HelloConfig{HelloMode::Compat}};
        srv.listen();
        srv.accept(5000ms);
        server_caps = srv.negotiated_capabilities();
        // v1-compat なのでクライアントの送ったメッセージを受信して応答できる
        Message msg = srv.receive(3000ms);
        EXPECT_EQ(msg.as_string_view(), "v1msg");
        srv.send(Message{std::string_view{"v1reply"}});
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    // v1.0.0 クライアント模倣
    const std::string response = legacy_connect_and_send(name, "v1msg", /*read_response=*/true);
    EXPECT_EQ(response, "v1reply");

    srv_fut.get();

    EXPECT_TRUE(server_caps.v1_compat);
    EXPECT_TRUE(server_caps.is_v1_compat());
    EXPECT_FALSE(server_caps.is_legacy_v1());
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-007 NegotiationStrictRejectsV1Client
// v1.0.0 クライアント + サーバー Strict → ConnectionRejected
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, NegotiationStrictRejectsV1Client) {
    const auto name = unique_pipe("strict_v1cli");

    std::exception_ptr srv_ex;
    auto srv_fut = std::async(std::launch::async, [&]() {
        PipeServer srv{name, 65536, PipeAcl::Default, "", HelloConfig{HelloMode::Strict}};
        srv.listen();
        try {
            srv.accept(5000ms);
        } catch (...) {
            srv_ex = std::current_exception();
        }
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    // v1.0.0 クライアントを接続（サーバーは拒否するため read_response=false）
    EXPECT_NO_THROW(legacy_connect_and_send(name, "v1msg", /*read_response=*/false));

    srv_fut.get();

    ASSERT_NE(srv_ex, nullptr);
    try {
        std::rethrow_exception(srv_ex);
    } catch (const PipeException& e) {
        EXPECT_EQ(e.code().value(),
                  static_cast<int>(PipeErrorCode::ConnectionRejected))
            << "Expected ConnectionRejected, got: " << e.what();
    } catch (...) {
        FAIL() << "Expected PipeException(ConnectionRejected)";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-008 NegotiationSkipMode
// 双方 Skip → HELLO 交換なし、通常メッセージ正常
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, NegotiationSkipMode) {
    const auto name = unique_pipe("skip_mode");

    auto srv_fut = run_server_async(name, HelloConfig{HelloMode::Skip},
        [](PipeServer& srv) {
            Message msg = srv.receive(3000ms);
            EXPECT_EQ(msg.as_string_view(), "ping");
            srv.send(Message{std::string_view{"pong"}});
        });

    std::this_thread::sleep_for(50ms);

    PipeClient cli{name, 65536, HelloConfig{HelloMode::Skip}};
    cli.connect(3000ms);
    cli.send(Message{std::string_view{"ping"}});
    Message reply = cli.receive(3000ms);
    EXPECT_EQ(reply.as_string_view(), "pong");
    cli.close();
    srv_fut.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-009 NegotiationCompatTimeout
// Compat + hello_timeout=50ms + Skip クライアント → フォールバック（v1_compat=false）
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, NegotiationCompatTimeout) {
    const auto name = unique_pipe("compat_timeout");

    HelloConfig srv_cfg{HelloMode::Compat, 50ms, 0};
    NegotiatedCapabilities server_caps;
    auto srv_fut = std::async(std::launch::async, [&]() {
        PipeServer srv{name, 65536, PipeAcl::Default, "", srv_cfg};
        srv.listen();
        srv.accept(5000ms);  // タイムアウト後 fallback で返る
        server_caps = srv.negotiated_capabilities();
        // 通常フレームを受信（バッファ済みフレームから返る）
        Message msg = srv.receive(3000ms);
        EXPECT_EQ(msg.as_string_view(), "hello");
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    // Skip クライアント → 接続後すぐ通常フレームを送信
    PipeClient cli{name, 65536, HelloConfig{HelloMode::Skip}};
    cli.connect(3000ms);
    cli.send(Message{std::string_view{"hello"}});
    cli.close();

    srv_fut.get();

    // Skip クライアントは v1.1.0 の通常フレームを送るので v1_compat=false
    EXPECT_FALSE(server_caps.v1_compat);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-010 NegotiationStrictTimeout
// Strict + hello_timeout=50ms + Skip クライアント → ConnectionRejected
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, NegotiationStrictTimeout) {
    const auto name = unique_pipe("strict_timeout");

    HelloConfig srv_cfg{HelloMode::Strict, 50ms, 0};
    std::exception_ptr srv_ex;
    auto srv_fut = std::async(std::launch::async, [&]() {
        PipeServer srv{name, 65536, PipeAcl::Default, "", srv_cfg};
        srv.listen();
        try {
            srv.accept(5000ms);
        } catch (...) {
            srv_ex = std::current_exception();
        }
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    // Skip クライアント → HELLO を送らない
    PipeClient cli{name, 65536, HelloConfig{HelloMode::Skip}};
    EXPECT_NO_THROW(cli.connect(3000ms));
    // サーバー側が拒否するまで短時間待機
    std::this_thread::sleep_for(200ms);
    cli.close();

    srv_fut.get();

    ASSERT_NE(srv_ex, nullptr) << "Server should have thrown ConnectionRejected";
    try {
        std::rethrow_exception(srv_ex);
    } catch (const PipeException& e) {
        EXPECT_EQ(e.code().value(),
                  static_cast<int>(PipeErrorCode::ConnectionRejected))
            << "Expected ConnectionRejected, got: " << e.what();
    } catch (...) {
        FAIL() << "Expected PipeException(ConnectionRejected)";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-011 BitAndNegotiation
// server_bitmap=0x0F, client_bitmap=0x05 → negotiated=0x05
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, BitAndNegotiation) {
    const auto name = unique_pipe("bitand");

    HelloConfig srv_cfg{HelloMode::Compat, 500ms, 0x0F};
    HelloConfig cli_cfg{HelloMode::Compat, 500ms, 0x05};

    NegotiatedCapabilities server_caps;
    NegotiatedCapabilities client_caps;

    auto srv_fut = run_server_async(name, srv_cfg,
        [&server_caps](PipeServer& srv) {
            server_caps = srv.negotiated_capabilities();
            srv.send(Message{std::string_view{"go"}});
        });

    std::this_thread::sleep_for(50ms);

    PipeClient cli{name, 65536, cli_cfg};
    cli.connect(3000ms);
    client_caps = cli.negotiated_capabilities();
    Message m = cli.receive(3000ms);
    EXPECT_EQ(m.as_string_view(), "go");
    cli.close();
    srv_fut.get();

    EXPECT_EQ(server_caps.bitmap, 0x05u);
    EXPECT_EQ(client_caps.bitmap, 0x05u);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-012 OnHelloCompleteCallback
// on_hello_complete が accept() 後に呼び出される
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, OnHelloCompleteCallback) {
    const auto name = unique_pipe("on_hello_cb");

    bool callback_called = false;
    NegotiatedCapabilities callback_caps;

    auto srv_fut = std::async(std::launch::async, [&]() {
        PipeServer srv{name, 65536, PipeAcl::Default, "", HelloConfig{HelloMode::Compat}};
        srv.on_hello_complete = [&](const NegotiatedCapabilities& caps) {
            callback_called = true;
            callback_caps   = caps;
        };
        srv.listen();
        srv.accept(5000ms);
        srv.send(Message{std::string_view{"done"}});
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    PipeClient cli{name, 65536, HelloConfig{HelloMode::Compat}};
    cli.connect(3000ms);
    Message m = cli.receive(3000ms);
    EXPECT_EQ(m.as_string_view(), "done");
    cli.close();
    srv_fut.get();

    EXPECT_TRUE(callback_called) << "on_hello_complete was not called";
    EXPECT_EQ(callback_caps.bitmap, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-013 NormalMessageAfterHello
// HELLO 後に通常メッセージ 10 往復が正常に動作する
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, NormalMessageAfterHello) {
    const auto name = unique_pipe("msg10_hello");

    auto srv_fut = run_server_async(name, HelloConfig{HelloMode::Compat},
        [](PipeServer& srv) {
            for (int i = 0; i < 10; ++i) {
                Message msg = srv.receive(3000ms);
                std::string reply = "reply_" + std::to_string(i);
                srv.send(Message{std::string_view{reply}});
            }
        });

    std::this_thread::sleep_for(50ms);

    PipeClient cli{name, 65536, HelloConfig{HelloMode::Compat}};
    cli.connect(3000ms);
    for (int i = 0; i < 10; ++i) {
        std::string send_str = "msg_" + std::to_string(i);
        cli.send(Message{std::string_view{send_str}});
        Message reply = cli.receive(3000ms);
        EXPECT_EQ(reply.as_string_view(), "reply_" + std::to_string(i));
    }
    cli.close();
    srv_fut.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// T-CN-014 V1CompatNormalMessages
// v1-compat モードで通常メッセージ 10 往復が正常に動作する
// ─────────────────────────────────────────────────────────────────────────────
TEST(CapabilityNegotiationTest, V1CompatNormalMessages) {
    const auto name = unique_pipe("v1compat_10");

    // サーバー: Compat モード（v1.0.0 クライアントを受け入れる）
    auto srv_fut = std::async(std::launch::async, [&name]() {
        PipeServer srv{name, 65536, PipeAcl::Default, "", HelloConfig{HelloMode::Compat}};
        srv.listen();
        srv.accept(5000ms);
        // 最初のメッセージはバッファ済み、残り 9 は通常受信
        for (int i = 0; i < 10; ++i) {
            Message msg = srv.receive(3000ms);
            const std::string expected = "v1_" + std::to_string(i);
            EXPECT_EQ(msg.as_string_view(), expected);
            std::string reply = "r_" + std::to_string(i);
            srv.send(Message{std::string_view{reply}});
        }
        srv.close();
    });

    std::this_thread::sleep_for(50ms);

    // v1.0.0 クライアント: OS ネイティブ API で raw bytes を送受信
    RawLegacyClient raw{name, 3000};
    for (int i = 0; i < 10; ++i) {
        const std::string payload = "v1_" + std::to_string(i);
        send_v1_frame(raw, payload);
        const std::string expected_reply = "r_" + std::to_string(i);
        EXPECT_EQ(recv_v1_response(raw), expected_reply);
    }

    srv_fut.get();
}
