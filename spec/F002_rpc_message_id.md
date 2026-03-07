# F-002 詳細設計: message_id / リクエスト–レスポンス RPC

**作成日**: 2026-02-28
**対象バージョン**: v0.3.0（F-001 完了後）
**依存**: F-001（MultiPipeServer） — 先行推奨（独立実装は可能）

---

## 1. 現状と課題

現在のフレームに `message_id` フィールドがないため、以下が不可能:

1. **応答照合**: 複数スレッドが `send → receive` を並行実行すると応答を取り違える
2. **非同期 RPC**: `send_request()` して後から応答を受け取るパターンが組めない
3. **サーバー側ハンドラ**: リクエスト ID に基づいて正しいクライアントに返答できない

---

## 2. ワイヤーフォーマット変更

### 2.1 現行フォーマット (PROTOCOL_VERSION = 0x01, 16 bytes)

```
Offset  Size  Field
------  ----  -------------------------------------------
  0      4    magic[4]       {'P','I','P','E'}
  4      1    version        0x01
  5      1    flags          FLAG_* ビット
  6      2    reserved[2]    0x00 0x00
  8      4    payload_size   ペイロードバイト数 (LE)
 12      4    checksum       CRC-32C (LE)
------  ----
 合計   16
```

### 2.2 拡張フォーマット (PROTOCOL_VERSION = 0x02, 20 bytes)

```
Offset  Size  Field
------  ----  -------------------------------------------
  0      4    magic[4]       {'P','I','P','E'}
  4      1    version        0x02
  5      1    flags          FLAG_* ビット (FLAG_REQUEST 追加)
  6      2    reserved[2]    0x00 0x00
  8      4    payload_size   ペイロードサイズ (LE)          ← v0.01 と同オフセット
 12      4    checksum       CRC-32C (LE)                  ← v0.01 と同オフセット
 16      4    message_id     メッセージ ID (LE, 0 = ID なし)  ← 新規 (末尾追加)
------  ----
 合計   20
```

> **末尾追加の理由**: `payload_size` / `checksum` のオフセットを v0.01 と揃えることで、
> バージョン不一致時でもフレームサイズの計算が可能になり、将来的な lenient パーサーの実装余地を残せる。
> 一般原則「既存フィールドのオフセットを変えない」に従う。

**`message_id` 仕様**:
- `0x00000000`: ID なし（旧来の send/receive と同義）
- `0x00000001` 〜 `0xFFFFFFFE`: リクエスト/レスポンス対に使用
- `0xFFFFFFFF`: 予約

**`flags` 追加ビット**:
```cpp
FLAG_REQUEST  = 0x04;  // リクエスト (send_request 側が立てる)
FLAG_RESPONSE = 0x08;  // レスポンス (on_request ハンドラ返送時に立てる)
```

### 2.3 後方互換性方針

| 状況 | 動作 |
|---|---|
| v0.02 → v0.01 に接続 | `recv_frame` で `version != 0x02` → `InvalidMessage` 例外 |
| v0.01 → v0.02 に接続 | サーバー側で `version != 0x02` → `InvalidMessage` 例外 |
| 同一バージョン同士 | 正常動作 |

v0.1.0 リリース直後であり既存ユーザーへの影響は最小。移行ガイドを README に記載する。

### 2.4 FrameHeader 変更

```cpp
// frame_header.hpp (変更後)
#pragma pack(push, 1)
struct FrameHeader {
    uint8_t  magic[4];        // {'P','I','P','E'}
    uint8_t  version;         // PROTOCOL_VERSION (0x02)
    uint8_t  flags;           // FLAG_* ビット
    uint8_t  reserved[2];     // 0x00 (将来拡張)
    uint32_t payload_size;    // ペイロードサイズ (LE)  ← v0.01 と同オフセット (8)
    uint32_t checksum;        // CRC-32C (LE)          ← v0.01 と同オフセット (12)
    uint32_t message_id;      // メッセージ ID (LE); 0 = ID なし  ← 新規 (末尾, offset 16)
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 20, "FrameHeader must be exactly 20 bytes");

inline constexpr uint8_t PROTOCOL_VERSION  = 0x02;  // 変更
inline constexpr uint8_t FLAG_COMPRESSED   = 0x01;
inline constexpr uint8_t FLAG_ACK          = 0x02;
inline constexpr uint8_t FLAG_REQUEST      = 0x04;  // 新規
inline constexpr uint8_t FLAG_RESPONSE     = 0x08;  // 新規
inline constexpr uint32_t NO_MESSAGE_ID    = 0x00000000u;  // 新規
```

---

## 3. 新クラス設計: RpcPipeClient / RpcPipeServer

### 3.1 設計方針

**既存の `PipeClient` / `PipeServer` は変更しない**。  
RPC 機能を追加した `RpcPipeClient` / `RpcPipeServer` を新クラスとして追加する。

理由:
- `PipeClient` は同期 API として完結しており変更不要
- RPC クラスは背景受信スレッドを持つため、ライフサイクルが異なる
- 既存コードの破壊なし

```
PipeClient       ← 既存: 同期 send/receive
RpcPipeClient    ← 新規: PipeClient を継承or合成 + 背景受信スレッド + send_request()

PipeServer       ← 既存: 同期 send/receive
RpcPipeServer    ← 新規: PipeServer を継承or合成 + on_request() ハンドラ
```

**合成（HAS-A）を採用**:  
継承は `send`/`receive` の意味が変わるため混乱を招く。合成で包む。

> **『公開 API は変更しない』の正確な蒸留**:  
> `PipeClient` / `PipeServer` の **公開ヘッダ (.hpp) と外部向け API は変更しない**。  
> ただし、内部実装の `pipe_client.cpp` / `pipe_server.cpp` が使う共通基盤レイヤー（`send_frame` / `recv_frame`）はフレームヘッダ拡張に伴って変更する。  
> これは「公開 API は不変更・内部実装の一部変更」であり、影響範囲は変更ファイル一覧（9 章）のとおり。

---

## 4. RpcPipeClient 公開 API

```cpp
// rpc_pipe_client.hpp
namespace pipeutil {

class PIPEUTIL_API RpcPipeClient {
public:
    explicit RpcPipeClient(std::string pipe_name,
                           std::size_t buffer_size = 65536);

    RpcPipeClient(const RpcPipeClient&)            = delete;
    RpcPipeClient& operator=(const RpcPipeClient&) = delete;
    RpcPipeClient(RpcPipeClient&&)                 noexcept;
    RpcPipeClient& operator=(RpcPipeClient&&)      noexcept;

    ~RpcPipeClient();

    // ─── ライフサイクル ──────────────────────────────────────────────

    /// サーバーに接続して背景受信スレッドを起動する
    /// 例外: PipeException (Timeout / NotFound / SystemError)
    void connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /// 接続を閉じ、背景スレッドを停止する (noexcept)
    void close() noexcept;

    // ─── 通常の同期送受信 (message_id = 0) ───────────────────────────

    void                send(const Message& msg);
    [[nodiscard]] Message receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ─── RPC: リクエスト–レスポンス ──────────────────────────────────

    /// フレームに message_id を付与して送信し、対応する応答を待つ
    /// timeout = 0 → 無限待機
    /// 例外: PipeException (Timeout / ConnectionReset / BrokenPipe)
    [[nodiscard]] Message send_request(
        const Message&            request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool               is_connected() const noexcept;
    [[nodiscard]] const std::string& pipe_name()    const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
```

---

## 5. RpcPipeServer 公開 API

```cpp
// rpc_pipe_server.hpp
namespace pipeutil {

class PIPEUTIL_API RpcPipeServer {
public:
    explicit RpcPipeServer(std::string pipe_name,
                           std::size_t buffer_size = 65536);

    RpcPipeServer(const RpcPipeServer&)            = delete;
    RpcPipeServer& operator=(const RpcPipeServer&) = delete;
    RpcPipeServer(RpcPipeServer&&)                 noexcept;
    RpcPipeServer& operator=(RpcPipeServer&&)      noexcept;

    ~RpcPipeServer();

    // ─── ライフサイクル ──────────────────────────────────────────────

    void listen();
    void accept(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    /// 接続後、背景スレッドでリクエストを受け取り handler を呼び出すループを開始
    /// handler の戻り値をレスポンスとして送信する
    ///
    /// run_in_background == false : stop() までブロック
    /// run_in_background == true  : 即座に返る
    using RequestHandler = std::function<Message(const Message& request)>;
    void serve_requests(RequestHandler handler,
                        bool run_in_background = false);

    /// serve_requests の呼び出し後は、受信主体が背景スレッドに移行するため、
    /// serve_requests 実行中に receive() / send() を直接呼び出すことは禁止する。
    /// 違反時の振る舞いは未定義 (UB 漏れ).

    /// serve_requests のループを停止し、背景スレッドの終了を待つ (noexcept)
    void stop() noexcept;

    void close() noexcept;

    // ─── 通常の同期送受信 (既存互換) ─────────────────────────────────

    void                send(const Message& msg);
    [[nodiscard]] Message receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ─── 状態照会 ─────────────────────────────────────────────────────

    [[nodiscard]] bool               is_listening()  const noexcept;
    [[nodiscard]] bool               is_connected()  const noexcept;
    [[nodiscard]] const std::string& pipe_name()     const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeutil
```

---

## 6. RpcPipeClient 内部設計

### 6.1 背景受信スレッド

```
connect() 後
│
└─ [receiver_thread]  (1本, 内部管理)
      ループ:
        recv_frame() → message_id を取り出す
          if message_id == 0:
            通常受信キュー (receive() 呼び出し元が取り出す)
          else:
            // find で存在確認→未登録 ID (タイムアウト後の遅延応答等) は破棄
            it = pending_map_.find(message_id)
            if it != pending_map_.end():
                it->second.set_value(msg)
                pending_map_.erase(it)
            else:
                /* 未登録 ID: 診断ログ対象、破棄 */

      ─ 接続切断 / close() ─
        pending_map_ の全 promise に PipeException を set_exception()
        stop_flag_ = true
```

### 6.2 send_request() 内部シーケンス

```cpp
Message RpcPipeClient::Impl::send_request(const Message& req, ms timeout) {
    // 1. message_id を採番 (atomic increment, 0 と 0xFFFFFFFF をスキップ)
    uint32_t id = next_id_.fetch_add(1, relaxed);
    if (id == 0 || id == 0xFFFFFFFF) id = next_id_.fetch_add(1, relaxed);

    // 2. promise を pending_map_ に登録 (mutex protected)
    std::promise<Message> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lk{pending_mutex_};
        pending_map_[id] = std::move(promise);
    }

    // 3. FLAG_REQUEST | message_id を付与してフレーム送信
    {
        std::lock_guard lk{io_mutex_};
        send_frame_with_id(req, id, FLAG_REQUEST);
    }

    // 4. 応答を待機 (タイムアウト付き)
    //    timeout == 0 は無限待機 (future.wait())
    //    timeout > 0 は wait_for() で期限付き待機
    if (timeout.count() == 0) {
        future.wait();  // 無限待機
    } else if (future.wait_for(timeout) == std::future_status::timeout) {
        // タイムアウト: pending_map_ から削除
        std::lock_guard lk{pending_mutex_};
        pending_map_.erase(id);
        throw PipeException{PipeErrorCode::Timeout, "send_request timed out"};
    }
    return future.get();  // ここで PipeException が再送出される場合もある
}
```

### 6.3 データ構造

```cpp
class RpcPipeClient::Impl {
    std::string                            pipe_name_;
    std::unique_ptr<detail::IPlatformPipe> platform_;
    std::mutex                             io_mutex_;      // 送信保護
    std::mutex                             pending_mutex_; // pending_map_ 保護
    std::thread                            receiver_thread_;
    std::atomic<bool>                      stop_flag_{false};

    std::atomic<uint32_t>                  next_id_{1};

    // 通常受信 (message_id == 0) キュー
    std::mutex                             recv_mutex_;
    std::condition_variable                recv_cv_;
    std::queue<Message>                    recv_queue_;

    // RPC 応答待ちテーブル
    std::unordered_map<uint32_t, std::promise<Message>> pending_map_;
};
```

---

## 7. RpcPipeServer 内部設計

### 7.1 serve_requests() のループ

```
accept() 後 → serve_requests(handler)
※ serve_requests 呼び出し後は、この接続に対する receive() / send() の直接呼び出しは禁止。
フレーム読取りの排他制御は内部 handler_thread が担当する。
│
└─ [handler_thread]
      ループ:
        recv_frame() → message_id, flags を取り出す
          if FLAG_REQUEST が立っている:
            resp = handler(msg)
            send_frame_with_id(resp, message_id, FLAG_RESPONSE)
          else:
            通常受信キューに入れる
```

---

## 8. Python バインディング

```python
# クライアント側
client = pipeutil.RpcPipeClient("my_pipe")
client.connect(timeout_ms=3000)
resp = client.send_request(pipeutil.Message(b'{"cmd":"ping"}'), timeout_ms=3000)
print(resp)  # → b'{"result":"pong"}'

# サーバー側
server = pipeutil.RpcPipeServer("my_pipe")
server.listen()
server.accept(timeout_ms=10000)

def handle_request(req: pipeutil.Message) -> pipeutil.Message:
    return pipeutil.Message(b'{"result":"pong"}')

server.serve_requests(handle_request, run_in_background=True)
```

### Python GIL 対応

`receiver_thread_` は C++ スレッドのため、Python callable を呼ぶ際に GIL を取得する必要がある。
`serve_requests` の handler が Python callable なら `PyGILState_Ensure / Release` でラップする。

---

## 9. 変更ファイル一覧

| ファイル | 変更内容 |
|---|---|
| `source/core/include/pipeutil/detail/frame_header.hpp` | ヘッダを 20 bytes に拡張、`PROTOCOL_VERSION=0x02`、`FLAG_REQUEST/RESPONSE`、`NO_MESSAGE_ID` 追加 |
| `source/core/src/pipe_server.cpp` | `send_frame` / `recv_frame` を 20-byte ヘッダ対応に更新 |
| `source/core/src/pipe_client.cpp` | 同上 |
| `source/core/include/pipeutil/rpc_pipe_client.hpp` | **新規** |
| `source/core/src/rpc_pipe_client.cpp` | **新規** |
| `source/core/include/pipeutil/rpc_pipe_server.hpp` | **新規** |
| `source/core/src/rpc_pipe_server.cpp` | **新規** |
| `source/core/include/pipeutil/pipeutil.hpp` | 新ヘッダを `#include` に追加 |
| `source/core/CMakeLists.txt` | 新 `.cpp` を `CORE_SOURCES` に追加 |
| `source/python/py_rpc_pipe_client.hpp/cpp` | **新規** |
| `source/python/py_rpc_pipe_server.hpp/cpp` | **新規** |
| `source/python/_pipeutil_module.cpp` | 新型を登録 |
| `tests/cpp/test_rpc.cpp` | **新規** |
| `tests/python/test_rpc.py` | **新規** |

---

## 10. テスト計画

### C++ テスト (`tests/cpp/test_rpc.cpp`)

| テスト名 | 検証内容 |
|---|---|
| `SendRequest_SingleRoundTrip` | send_request → 対応レスポンスが届くこと |
| `SendRequest_MultipleParallel` | 3 スレッドが並行 send_request しても応答が混在しないこと |
| `SendRequest_Timeout_ThrowsTimeout` | サーバーが応答しない場合に Timeout 例外が発生すること |
| `PlainSend_CoexistsWithRpc` | send()/receive() と send_request() が同一接続上で共存できること |
| `ServerDisconnect_RejectsAllPending` | 接続切断時に全 pending に ConnectionReset が届くこと |
| `FrameHeader_Size_Is20` | `sizeof(FrameHeader) == 20` の static_assert チェック |
| `Version_Mismatch_ThrowsInvalidMessage` | v0.01 フレームを v0.02 パーサーに渡すと InvalidMessage になること |

### Python テスト (`tests/python/test_rpc.py`)

| テスト名 | 検証内容 |
|---|---|
| `TestRpc::test_single_roundtrip` | Python による RPC ラウンドトリップ |
| `TestRpc::test_concurrent_requests` | 複数スレッドからの並行 send_request |
| `TestRpc::test_timeout_raises` | タイムアウト例外 |

---

## 11. 既知リスク・注意点

| リスク | 対策 |
|---|---|
| `next_id_` が `uint32_t` を一周 (wrap-around) した場合、古い pending と衝突する可能性 | 採番時に `pending_map_` に同一 ID が既に存在しないことをチェックする |
| receiver_thread が例外で死んだ場合 | 全 pending に `set_exception`、`stop_flag_` を立て `is_connected()` を false に |
| send_request + 通常 receive の混在: 応答フレームを通常 receive が拾ってしまう | `FLAG_RESPONSE` 立っているフレームは通常受信キューに入れない |
| フレームヘッダ 16→20 bytes: 旧バイナリとの互換なし | v0.2.0 から v0.3.0 への移行ガイドを README に記載 |
| Python GIL: `serve_requests` ハンドラがクラッシュ | C++ 側で例外キャッチ → PyErr_SetString で Python 例外に変換 |
