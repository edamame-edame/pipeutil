# F-006 詳細設計: 診断・メトリクス API

**作成日**: 2026-03-11
**対象バージョン**: v0.6.0
**依存**: F-001（MultiPipeServer）, F-002（RpcPipeClient）

---

## 1. 現状と課題

### 1.1 問題

本番稼働時のデバッグ・パフォーマンス分析に、送受信カウント・バイト数・ラウンドトリップレイテンシが必要。
現状はライブラリ外部で計測するしかなく、内部の状態（実際に何バイト送受信されたか、
RPC の平均応答時間はどれくらいか）を取得する手段がない。

```python
# 現状 — 外部計測は常に 2 重カウントのリスクがある
start = time.perf_counter()
resp = client.send_request(Message(b"ping"))
elapsed = time.perf_counter() - start
# これは Python オーバーヘッドを含む。C++ 内部のレイテンシとは異なる。
```

### 1.2 スコープ定義

| 項目 | 対応 |
|---|---|
| `PipeClient::stats()` / `reset_stats()` | ✅ 必須 |
| `PipeServer::stats()` / `reset_stats()` | ✅ 必須 |
| `RpcPipeClient::stats()` / `reset_stats()` | ✅ 必須（ラウンドトリップを含む） |
| `RpcPipeServer::stats()` / `reset_stats()` | ✅ 必須 |
| `MultiPipeServer::stats()` / `reset_stats()` | ✅ 必須（全接続合算） |
| Python バインディング（`PyPipeStats` 型） | ✅ 必須 |
| `ReconnectingPipeClient` の再接続メトリクス | ❌ 範囲外（`retry_count` プロパティで代替） |
| ログ出力・Prometheus エクスポート | ❌ 範囲外（ユーザー責務） |
| パーセンタイルレイテンシ（p99 など）| ❌ 範囲外 |

---

## 2. アーキテクチャ概観

```
source/core/include/pipeutil/
  pipe_stats.hpp           ← 新規: PipeStats 構造体
  pipe_client.hpp          ← 更新: stats() / reset_stats() 追加
  pipe_server.hpp          ← 更新: stats() / reset_stats() 追加
  rpc_pipe_client.hpp      ← 更新: stats() / reset_stats() 追加
  rpc_pipe_server.hpp      ← 更新: stats() / reset_stats() 追加
  multi_pipe_server.hpp    ← 更新: stats() / reset_stats() 追加
  pipeutil.hpp             ← 更新: pipe_stats.hpp を include 追加

source/core/src/
  pipe_client.cpp          ← 更新: Impl にカウンタ追加
  pipe_server.cpp          ← 更新: Impl にカウンタ追加
  rpc_pipe_client.cpp      ← 更新: Impl にカウンタ追加
  rpc_pipe_server.cpp      ← 更新: Impl にカウンタ追加
  multi_pipe_server.cpp    ← 更新: 各接続の stats を合算

source/python/
  py_pipe_stats.cpp        ← 新規: PyPipeStats 型定義
  py_pipe_client.cpp       ← 更新: stats / reset_stats メソッド
  py_pipe_server.cpp       ← 更新: stats / reset_stats メソッド
  py_rpc_pipe_client.cpp   ← 更新: stats / reset_stats メソッド
  py_rpc_pipe_server.cpp   ← 更新: stats / reset_stats メソッド
  py_multi_pipe_server.cpp ← 更新: stats / reset_stats メソッド
  _pipeutil_module.cpp     ← 更新: PyPipeStats 型を登録

python/pipeutil/
  __init__.pyi             ← 更新: PipeStats 型スタブ追加

tests/cpp/
  test_stats.cpp           ← 新規: 12 件

tests/python/
  test_stats.py            ← 新規: 15 件
```

### 2.1 設計原則

1. **ロックフリーカウンタ**: `std::atomic<uint64_t>` で加算（`memory_order_relaxed`）。
   `send` / `receive` のホットパスにロックを追加しない。
2. **スナップショット取得**: `stats()` は全フィールドをまとめて読み取るが、原子的な全フィールド一括取得は
   提供しない（`std::atomic` の `uint64_t` 個別読み取りで十分実用的）。
3. **リセット**: `reset_stats()` は全カウンタを `store(0)` するだけ。スレッドセーフ。
4. **ラウンドトリップ計測**: `RpcPipeClient` の `send_request()` でのみ計測する。
   計測は `std::chrono::steady_clock` を使用し、累積合計ナノ秒と計測回数を別々に保持する。
   平均は `stats()` 取得時に計算する。
5. **バイト数**: ペイロードのバイト数のみカウントする（フレームヘッダ 20 バイトは含まない）。
   ユーザーが意識するデータ量と一致させるため。
6. **MultiPipeServer の合算**: 現行実装は接続スレッドを `detach()` する運用のため、
   接続オブジェクトのポインタリストを保持しない。
   セッション終了時（`SlotGuard` デストラクタ内）に `accumulated_stats_`（mutex 保護）へ
   そのセッションの統計を加算する。`stats()` はこの累積バッファを返す。
   アクティブセッションの統計はセッション終了後に反映される。

---

## 3. C++ API 設計

### 3.1 `PipeStats` 構造体

```cpp
// <pipeutil/pipe_stats.hpp>
#pragma once
#include "pipeutil_export.hpp"
#include <cstdint>
#include <chrono>

namespace pipeutil {

/// 診断・メトリクス情報のスナップショット。
/// stats() が返す値オブジェクト（コピー可）。
struct PIPEUTIL_API PipeStats {
    // ─── 送受信カウンタ ──────────────────────────────────────────────
    /// 送信に成功したメッセージ数（例外なしで完了した send() の回数）
    std::uint64_t messages_sent     = 0;
    /// 受信に成功したメッセージ数（例外なしで完了した receive() の回数）
    std::uint64_t messages_received = 0;
    /// 送信したペイロードの総バイト数（フレームヘッダ除く）
    std::uint64_t bytes_sent        = 0;
    /// 受信したペイロードの総バイト数（フレームヘッダ除く）
    std::uint64_t bytes_received    = 0;
    /// 送受信中にキャッチした PipeException の総数（1 回の send/receive 呼び出し = 最大 1 カウント）
    std::uint64_t errors            = 0;

    // ─── RPC ラウンドトリップ（RpcPipeClient のみ有効） ──────────────
    /// send_request() が正常完了した回数
    std::uint64_t              rpc_calls           = 0;
    /// send_request() のラウンドトリップ合計時間（ナノ秒）
    /// avg_round_trip_ns = rtt_total_ns / rpc_calls で平均を得る
    std::uint64_t              rtt_total_ns        = 0;
    /// 最後の send_request() のラウンドトリップ時間（ナノ秒）
    /// rpc_calls == 0 の場合は 0
    std::uint64_t              rtt_last_ns         = 0;

    // ─── 便利メソッド ───────────────────────────────────────────────
    /// send_request() のラウンドトリップ平均（ナノ秒）を返す。
    /// rpc_calls == 0 の場合は 0 を返す。
    [[nodiscard]] std::uint64_t avg_round_trip_ns() const noexcept {
        return rpc_calls > 0 ? rtt_total_ns / rpc_calls : 0;
    }

    /// stats の加算演算子（MultiPipeServer が複数接続を合算する用途で使用）
    PipeStats& operator+=(const PipeStats& rhs) noexcept;
};

/// 加算演算子（非メンバー）
PIPEUTIL_API PipeStats operator+(PipeStats lhs, const PipeStats& rhs) noexcept;

} // namespace pipeutil
```

**なぜ `avg_round_trip` をフィールドではなくメソッドで提供するか**:
合計と回数を別保持することで `reset_stats()` 後も再計算できる。
Python 側では読み取り専用プロパティとして公開する（§4.2 参照）。

### 3.2 `PipeStats::operator+=` の実装

```cpp
// pipe_stats.cpp
PipeStats& PipeStats::operator+=(const PipeStats& rhs) noexcept {
    messages_sent     += rhs.messages_sent;
    messages_received += rhs.messages_received;
    bytes_sent        += rhs.bytes_sent;
    bytes_received    += rhs.bytes_received;
    errors            += rhs.errors;
    rpc_calls         += rhs.rpc_calls;
    rtt_total_ns      += rhs.rtt_total_ns;
    // rtt_last_ns は最後のもの（加算の意味がないため lhs 優先で変更しない）
    return *this;
}
```

### 3.3 `PipeClient` / `PipeServer` への追加

```cpp
// pipe_client.hpp (追加分のみ抜粋)
#include "pipe_stats.hpp"

class PIPEUTIL_API PipeClient {
    // ... 既存メンバー ...

    /// 計測スナップショットを返す（noexcept）
    [[nodiscard]] PipeStats stats() const noexcept;
    /// 全カウンタを 0 にリセットする（noexcept）
    void reset_stats() noexcept;
};
```

`PipeServer` も同じシグネチャを追加する。

### 3.4 `RpcPipeClient` への追加

```cpp
// rpc_pipe_client.hpp (追加分のみ抜粋)
    /// 計測スナップショットを返す（noexcept）
    [[nodiscard]] PipeStats stats() const noexcept;
    /// 全カウンタを 0 にリセットする（noexcept）
    void reset_stats() noexcept;
```

`RpcPipeServer` も同じシグネチャを追加する。

### 3.5 `MultiPipeServer` への追加

```cpp
// multi_pipe_server.hpp (追加分のみ抜粋)
    /// 全接続の stats を合算したスナップショットを返す（noexcept）
    [[nodiscard]] PipeStats stats() const noexcept;
    /// 全接続の stats を一斉リセットする（noexcept）
    void reset_stats() noexcept;
```

---

## 4. C++ 実装詳細

### 4.1 `PipeClient::Impl` のカウンタ設計

```cpp
// pipe_client.cpp（PipeClient::Impl の追加メンバー）
class PipeClient::Impl {
    // ... 既存メンバー（platform_, io_mutex_ など）...

    // ─── 統計カウンタ（memory_order_relaxed で十分：最終一貫性で OK） ──
    std::atomic<uint64_t> stat_msgs_sent_     {0};
    std::atomic<uint64_t> stat_msgs_recv_     {0};
    std::atomic<uint64_t> stat_bytes_sent_    {0};
    std::atomic<uint64_t> stat_bytes_recv_    {0};
    std::atomic<uint64_t> stat_errors_        {0};
    // RPC 用（PipeClient では 0 のまま）
    std::atomic<uint64_t> stat_rpc_calls_     {0};
    std::atomic<uint64_t> stat_rtt_total_ns_  {0};
    std::atomic<uint64_t> stat_rtt_last_ns_   {0};
};
```

**`send()` カウンタ更新例**:

```cpp
void send(const Message& msg) {
    std::lock_guard<std::mutex> lk(io_mutex_);
    try {
        detail::send_frame(*platform_, msg);
        // 成功時のみカウント
        stat_msgs_sent_.fetch_add(1, std::memory_order_relaxed);
        stat_bytes_sent_.fetch_add(msg.size(), std::memory_order_relaxed);
    } catch (const PipeException&) {
        // PipeException のみカウント（std::bad_alloc 等は対象外）
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
}
```

**`stats()` 実装**:

```cpp
PipeStats PipeClient::Impl::stats_snapshot() const noexcept {
    PipeStats s;
    s.messages_sent     = stat_msgs_sent_.load(std::memory_order_relaxed);
    s.messages_received = stat_msgs_recv_.load(std::memory_order_relaxed);
    s.bytes_sent        = stat_bytes_sent_.load(std::memory_order_relaxed);
    s.bytes_received    = stat_bytes_recv_.load(std::memory_order_relaxed);
    s.errors            = stat_errors_.load(std::memory_order_relaxed);
    s.rpc_calls         = stat_rpc_calls_.load(std::memory_order_relaxed);
    s.rtt_total_ns      = stat_rtt_total_ns_.load(std::memory_order_relaxed);
    s.rtt_last_ns       = stat_rtt_last_ns_.load(std::memory_order_relaxed);
    return s;
}

void PipeClient::Impl::reset_stats() noexcept {
    stat_msgs_sent_.store(0, std::memory_order_relaxed);
    stat_msgs_recv_.store(0, std::memory_order_relaxed);
    stat_bytes_sent_.store(0, std::memory_order_relaxed);
    stat_bytes_recv_.store(0, std::memory_order_relaxed);
    stat_errors_.store(0, std::memory_order_relaxed);
    stat_rpc_calls_.store(0, std::memory_order_relaxed);
    stat_rtt_total_ns_.store(0, std::memory_order_relaxed);
    stat_rtt_last_ns_.store(0, std::memory_order_relaxed);
}
```

### 4.2 `RpcPipeClient::Impl` のラウンドトリップ計測

```cpp
// rpc_pipe_client.cpp — send_request の変更部分
Message send_request(const Message& req, std::chrono::milliseconds timeout) {
    // ... 既存のリクエスト送信ロジック ...

    const auto t0 = std::chrono::steady_clock::now();  // 計測開始

    try {
        // ... 既存の future.get() / 応答待ち ...
        const auto t1 = std::chrono::steady_clock::now();
        const auto rtt_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

        // 成功時にカウント
        stat_msgs_sent_.fetch_add(1, std::memory_order_relaxed);
        stat_bytes_sent_.fetch_add(req.size(), std::memory_order_relaxed);
        stat_msgs_recv_.fetch_add(1, std::memory_order_relaxed);     // 応答も 1 メッセージ
        stat_bytes_recv_.fetch_add(response.size(), std::memory_order_relaxed);
        stat_rpc_calls_.fetch_add(1, std::memory_order_relaxed);
        stat_rtt_total_ns_.fetch_add(rtt_ns, std::memory_order_relaxed);
        stat_rtt_last_ns_.store(rtt_ns, std::memory_order_relaxed);

        return response;
    } catch (const PipeException&) {
        // PipeException のみカウント（std::bad_alloc 等は対象外）
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
}
```

**注意事項**:
- `send_request` の `messages_sent` / `bytes_sent` / `messages_received` / `bytes_received` は
  RPC リクエスト・レスポンスのペアもカウントする（通常の `send` / `receive` と区別しない）。
- `send()` と `send_request()` の `bytes_sent` は統合されるため、
  Python 側ユーザーは `rpc_calls` フィールドで内訳を確認できる。

### 4.3 `MultiPipeServer` の合算

現行実装は接続スレッドを `detach()` するため、`active_connections_` コンテナを持たない
（`active_count_: std::atomic<std::size_t>` + `SlotGuard` RAII で管理）。
接続終了済みセッションの統計を失わないよう、`Impl` に累積バッファを追加し、
`SlotGuard` デストラクタで取り込む設計とする。

```cpp
// multi_pipe_server.cpp — MultiPipeServer::Impl 追加フィールド
struct MultiPipeServer::Impl {
    // ... 既存フィールド (active_count_, done_cv_, 等) ...

    // 終了済みセッション統計の累積バッファ（mutex で保護）
    mutable std::mutex accumulated_mutex_;
    PipeStats          accumulated_stats_{};
};

// SlotGuard デストラクタ — セッション終了時に統計を取り込む
SlotGuard::~SlotGuard() {
    {
        // セッション (PipeServer conn) の統計を累積バッファへ加算
        std::lock_guard lk{impl_.accumulated_mutex_};
        impl_.accumulated_stats_ += conn_.stats();
    }
    impl_.active_count_.fetch_sub(1, std::memory_order_acq_rel);
    impl_.done_cv_.notify_all();
}

// stats() — 累積バッファのスナップショットを返す
PipeStats MultiPipeServer::stats() const noexcept {
    // detach 運用のためアクティブセッションのポインタを保持しない。
    // 統計はセッション終了時に accumulated_stats_ へ取り込まれ、
    // アクティブセッションの現在値はセッションが閉じると反映される。
    std::lock_guard lk{impl_->accumulated_mutex_};
    return impl_->accumulated_stats_;
}

// reset_stats() — 累積バッファのみクリア（アクティブセッションには作用しない）
void MultiPipeServer::reset_stats() noexcept {
    std::lock_guard lk{impl_->accumulated_mutex_};
    impl_->accumulated_stats_ = PipeStats{};
}
```

> **設計上のトレードオフ**: アクティブセッションの統計は、そのセッションが
> 閉じるまで `stats()` に反映されない。長寿命の接続が多い環境では
> `stats()` が過去のセッション分しか返さない点に注意すること
> （短命接続が多いサーバー監視用途では実用上問題ない）。

---

## 5. Python バインディング設計

### 5.1 `PyPipeStats` 型

```
source/python/py_pipe_stats.cpp  ← 新規
```

`PipeStats` を Python オブジェクトとして公開する読み取り専用データクラス。

```python
# Python からの見え方
stats = client.stats()

# 基本フィールド（読み取り専用プロパティ）
stats.messages_sent       # int
stats.messages_received   # int
stats.bytes_sent          # int
stats.bytes_received      # int
stats.errors              # int

# RPC フィールド（PipeClient / PipeServer では常に 0）
stats.rpc_calls           # int
stats.rtt_total_ns        # int
stats.rtt_last_ns         # int
stats.avg_round_trip_ns   # int (rtt_total_ns // rpc_calls、rpc_calls == 0 なら 0)

# 文字列表現
repr(stats)
# → 'PipeStats(sent=10, recv=8, bytes_sent=1024, bytes_received=820, errors=0)'
```

**`PyPipeStats` の内部構造（C 型定義）**:

```cpp
typedef struct {
    PyObject_HEAD
    pipeutil::PipeStats stats;   // 値コピー（snap shot）
} PyPipeStats;
```

`PyPipeStats` は不変オブジェクトとして扱う（C++ の `PipeStats` は値型のため、
取得時点のスナップショットを保持すれば十分）。

### 5.2 各クラスへのメソッド追加

`PipeClient`, `PipeServer`, `RpcPipeClient`, `RpcPipeServer`, `MultiPipeServer` に
以下のメソッドを追加する。

```python
def stats(self) -> PipeStats: ...
def reset_stats(self) -> None: ...
```

### 5.3 `__init__.pyi` への型スタブ追加

```python
# ─── PipeStats ───────────────────────────────────────────────────────

class PipeStats:
    """診断・メトリクス情報のスナップショット。stats() が返す読み取り専用オブジェクト。"""

    @property
    def messages_sent(self) -> int:
        """送信に成功したメッセージ数。"""
        ...

    @property
    def messages_received(self) -> int:
        """受信に成功したメッセージ数。"""
        ...

    @property
    def bytes_sent(self) -> int:
        """送信したペイロードの総バイト数（フレームヘッダ除く）。"""
        ...

    @property
    def bytes_received(self) -> int:
        """受信したペイロードの総バイト数（フレームヘッダ除く）。"""
        ...

    @property
    def errors(self) -> int:
        """送受信中に発生した PipeError の総数。"""
        ...

    @property
    def rpc_calls(self) -> int:
        """send_request() が正常完了した回数（PipeClient / PipeServer では常に 0）。"""
        ...

    @property
    def rtt_total_ns(self) -> int:
        """send_request() のラウンドトリップ合計時間（ナノ秒）。"""
        ...

    @property
    def rtt_last_ns(self) -> int:
        """最後の send_request() のラウンドトリップ時間（ナノ秒）。"""
        ...

    @property
    def avg_round_trip_ns(self) -> int:
        """send_request() のラウンドトリップ平均（ナノ秒）。rpc_calls == 0 なら 0。"""
        ...

    def __repr__(self) -> str: ...
```

また `PipeClient`, `PipeServer`, `RpcPipeClient`, `RpcPipeServer`, `MultiPipeServer` の
型スタブブロックに以下を追加する。

```python
def stats(self) -> PipeStats:
    """計測スナップショットを返す（ロックなし）。"""
    ...

def reset_stats(self) -> None:
    """全カウンタを 0 にリセットする（ロックなし）。"""
    ...
```

---

## 6. スレッド安全性

| 操作 | 安全性 | 理由 |
|---|:---:|---|
| `send()` / `receive()` 中のカウンタ加算 | ✅ | `std::atomic` の `fetch_add(relaxed)` |
| `stats()` の読み取り | ✅ | 各フィールドを `load(relaxed)` で読み取る。全フィールド一括原子性は保証しない |
| `reset_stats()` のリセット | ✅ | 各フィールドを `store(0, relaxed)` でリセット |
| `stats()` と `reset_stats()` の並走 | ✅ | モニタリング用途では TOCTOU が問題にならない（診断情報のため） |

**フィールド間の原子性について**: `stats()` はすべてのフィールドを一括で原子的に取得しない。
並走する `send` / `receive` が進行中の場合、`messages_sent` と `bytes_sent` がわずかにずれる
スナップショットになりうる。これは診断・モニタリング用途では許容範囲であり、
過剰な同期コストを払ってまで全フィールド原子性を保証する必要はない。

---

## 7. 使用例

### 7.1 基本使用（Python）

```python
import pipeutil

client = pipeutil.PipeClient("my_pipe")
client.connect()

client.send(pipeutil.Message(b"hello"))
reply = client.receive()

stats = client.stats()
print(f"送信: {stats.messages_sent} メッセージ, {stats.bytes_sent} バイト")
print(f"受信: {stats.messages_received} メッセージ, {stats.bytes_received} バイト")
print(f"エラー: {stats.errors} 回")
```

### 7.2 RPC ラウンドトリップ計測

```python
import pipeutil

client = pipeutil.RpcPipeClient("my_pipe")
client.connect()

for _ in range(100):
    resp = client.send_request(pipeutil.Message(b"ping"))

stats = client.stats()
print(f"RPC 呼び出し数: {stats.rpc_calls}")
print(f"平均ラウンドトリップ: {stats.avg_round_trip_ns / 1_000_000:.2f} ms")
print(f"最後のラウンドトリップ: {stats.rtt_last_ns / 1_000:.1f} µs")
```

### 7.3 定期モニタリング（差分計算）

```python
import time
import pipeutil

client = pipeutil.PipeClient("my_pipe")
client.connect()

prev = client.stats()
while True:
    time.sleep(1.0)
    cur = client.stats()
    msgs_per_sec = cur.messages_sent - prev.messages_sent
    mbytes_per_sec = (cur.bytes_sent - prev.bytes_sent) / 1_000_000
    print(f"スループット: {msgs_per_sec} msg/s, {mbytes_per_sec:.2f} MB/s")
    prev = cur
```

### 7.4 C++ 使用例

```cpp
#include <pipeutil/pipeutil.hpp>
#include <iostream>

int main() {
    pipeutil::PipeClient client{"my_pipe"};
    client.connect();

    for (int i = 0; i < 10; ++i) {
        client.send(pipeutil::Message{"hello"});
        client.receive();
    }

    auto s = client.stats();
    std::cout << "sent=" << s.messages_sent
              << " bytes=" << s.bytes_sent
              << " avg_rtt=" << s.avg_round_trip_ns() << "ns\n";

    client.reset_stats();
    return 0;
}
```

---

## 8. ファイル構成と実装概要

### 8.1 新規ファイル

```
source/core/include/pipeutil/pipe_stats.hpp
  PipeStats 構造体（フィールド定義 + avg_round_trip_ns() + operator+=）

source/core/src/pipe_stats.cpp
  PipeStats::operator+= / operator+ の実装

source/python/py_pipe_stats.cpp
  PyPipeStats 型定義（読み取り専用プロパティ、__repr__）

tests/cpp/test_stats.cpp
  C++ ユニットテスト（12 件）

tests/python/test_stats.py
  Python テスト（15 件）
```

### 8.2 更新ファイル

```
source/core/include/pipeutil/pipeutil.hpp
  → pipe_stats.hpp を include 追加

source/core/include/pipeutil/pipe_client.hpp
source/core/include/pipeutil/pipe_server.hpp
source/core/include/pipeutil/rpc_pipe_client.hpp
source/core/include/pipeutil/rpc_pipe_server.hpp
source/core/include/pipeutil/multi_pipe_server.hpp
  → stats() / reset_stats() 宣言を追加

source/core/src/pipe_client.cpp
source/core/src/pipe_server.cpp
source/core/src/rpc_pipe_client.cpp
source/core/src/rpc_pipe_server.cpp
source/core/src/multi_pipe_server.cpp
  → Impl にカウンタ atomic を追加、send/receive 内でカウント

source/python/py_pipe_client.cpp
source/python/py_pipe_server.cpp
source/python/py_rpc_pipe_client.cpp
source/python/py_rpc_pipe_server.cpp
source/python/py_multi_pipe_server.cpp
  → stats / reset_stats メソッドを PyMethodDef に追加

source/python/_pipeutil_module.cpp
  → PyPipeStats 型を PyModule_AddObjectRef で登録

python/pipeutil/__init__.pyi
  → PipeStats 型スタブ、各クラスへのメソッドスタブを追加
```

---

## 9. 型スタブ全文（`__init__.pyi` 追記分）

```python
# ─── PipeStats ────────────────────────────────────────────────────────────────

class PipeStats:
    """診断・メトリクス情報のスナップショット。

    stats() が返す読み取り専用オブジェクト。
    取得時点の値をコピーして保持するため、その後の send/receive の影響を受けない。
    """

    @property
    def messages_sent(self) -> int:
        """送信に成功したメッセージ数。"""
        ...

    @property
    def messages_received(self) -> int:
        """受信に成功したメッセージ数。"""
        ...

    @property
    def bytes_sent(self) -> int:
        """送信したペイロードの総バイト数（フレームヘッダ 20 バイト除く）。"""
        ...

    @property
    def bytes_received(self) -> int:
        """受信したペイロードの総バイト数（フレームヘッダ 20 バイト除く）。"""
        ...

    @property
    def errors(self) -> int:
        """送受信中に発生した PipeError の総数（1 呼び出し = 最大 1 カウント）。"""
        ...

    @property
    def rpc_calls(self) -> int:
        """send_request() が正常完了した回数。
        PipeClient / PipeServer では常に 0。"""
        ...

    @property
    def rtt_total_ns(self) -> int:
        """send_request() のラウンドトリップ合計時間（ナノ秒）。"""
        ...

    @property
    def rtt_last_ns(self) -> int:
        """最後の send_request() のラウンドトリップ時間（ナノ秒）。
        rpc_calls == 0 の場合は 0。"""
        ...

    @property
    def avg_round_trip_ns(self) -> int:
        """send_request() のラウンドトリップ平均（ナノ秒）。
        rpc_calls == 0 の場合は 0。"""
        ...

    def __repr__(self) -> str: ...
```

---

## 10. テスト設計

### 10.1 C++ テスト（`tests/cpp/test_stats.cpp`）

| # | テスト名 | 概要 |
|---|---|---|
| TC1 | `stats_initial_zero` | 初期状態で全フィールドが 0 であること |
| TC2 | `stats_send_increments` | `send()` 成功後に `messages_sent` / `bytes_sent` がインクリメントされること |
| TC3 | `stats_receive_increments` | `receive()` 成功後に `messages_received` / `bytes_received` がインクリメントされること |
| TC4 | `stats_error_increments` | `send()` 失敗時に `errors` がインクリメントされること |
| TC5 | `stats_reset` | `reset_stats()` 後に全フィールドが 0 に戻ること |
| TC6 | `stats_rpc_round_trip` | `send_request()` 成功後に `rpc_calls` / `rtt_last_ns` / `avg_round_trip_ns()` が正しい値であること |
| TC7 | `stats_rpc_total_ns_accumulates` | 複数回 `send_request()` 後に `rtt_total_ns` が累積されること |
| TC8 | `stats_rpc_error_no_rtt` | `send_request()` でタイムアウトした場合に `rpc_calls` はインクリメントされないこと |
| TC9 | `stats_thread_safe_send` | 複数スレッドから並列 `send()` しても `messages_sent` が正確にカウントされること |
| TC10 | `stats_plus_operator` | `operator+` で 2 つの `PipeStats` を加算できること |
| TC11 | `stats_pipe_client_avg_rtt_zero` | `PipeClient` の `avg_round_trip_ns()` は常に 0 であること |
| TC12 | `stats_multi_server_aggregation` | セッション終了後に `MultiPipeServer::stats()` が累積統計を返すこと |

### 10.2 Python テスト（`tests/python/test_stats.py`）

モック対象: `PipeClient.send` / `receive` / `stats` を `unittest.mock` で差し替える（結合テストは行わない）。

| # | テスト名 | 概要 |
|---|---|---|
| T1 | `test_stats_returns_pipe_stats` | `client.stats()` が `PipeStats` 型を返すこと |
| T2 | `test_stats_initial` | 接続直後の stats が全フィールド 0 であること |
| T3 | `test_messages_sent_increments` | `send()` 後に `messages_sent` が 1 増えること |
| T4 | `test_bytes_sent_increments` | 100 バイト送信後に `bytes_sent >= 100` であること |
| T5 | `test_messages_received_increments` | `receive()` 後に `messages_received` が 1 増えること |
| T6 | `test_errors_on_broken_pipe` | `BrokenPipeError` 発生後に `errors` が 1 増えること |
| T7 | `test_reset_stats` | `reset_stats()` 後に全フィールドが 0 に戻ること |
| T8 | `test_rpc_calls_increment` | `send_request()` 後に `rpc_calls` が 1 増えること |
| T9 | `test_rtt_last_ns_nonzero` | `send_request()` 後に `rtt_last_ns > 0` であること |
| T10 | `test_avg_round_trip_zero_when_no_calls` | `rpc_calls == 0` の場合 `avg_round_trip_ns == 0` であること |
| T11 | `test_avg_round_trip_equals_total_div_calls` | `avg_round_trip_ns == rtt_total_ns // rpc_calls` であること |
| T12 | `test_pipe_client_rpc_fields_always_zero` | `PipeClient.stats()` の RPC フィールドは常に 0 であること |
| T13 | `test_stats_snapshot_independence` | `stats()` 取得後の送信は取得済みスナップショットに影響しないこと |
| T14 | `test_pipe_stats_repr` | `repr(stats)` が `PipeStats(` で始まること |
| T15 | `test_server_stats` | `PipeServer.stats()` / `reset_stats()` が呼び出せること |

---

## 11. 制約・注意事項

1. **スナップショットの非原子性**: `stats()` は全フィールドを一括原子取得しない。
   並走する I/O が進行中の場合、`messages_sent` と `bytes_sent` がわずかにずれる。
   これは診断・モニタリング用途では許容範囲。

2. **バイト数はペイロードのみ**: `bytes_sent` / `bytes_received` はフレームヘッダ（20 バイト）を含まない。
   ユーザーが送受信したデータ量と一致させるため。実際のネットワーク帯域消費量を計算する場合は
   `messages_sent * 20 + bytes_sent` とする（フレームヘッダ分を補正）。

3. **`rtt_last_ns` の合算演算子**: `operator+=` では `rtt_last_ns` を加算しない（意味がない）。
   `MultiPipeServer::stats()` で合算する場合、`rtt_last_ns` は lhs の値を維持する。
   `rtt_total_ns / rpc_calls` で平均を計算すること。

4. **エラーカウントの定義**: `errors` は `send()` / `receive()` / `send_request()` の
   1 回の呼び出しで例外が 1 つ発生したら 1 カウント。`PipeException` 以外（`std::bad_alloc` 等）は
   カウントしない（`catch(const PipeException&)` ブロック内でカウント）。

5. **`reset_stats()` の途中状態**: `reset_stats()` 直後に別スレッドが `send()` を完了した場合、
   `messages_sent == 1` になることがある。これは原子性の問題ではなく、
   「リセット前の操作がリセット後にカウントされた」という正常なレースコンディションである。
   定期的にリセットする用途（差分計算）ではこの挙動を考慮すること。

6. **`MultiPipeServer::stats()` のリアルタイム性**: アクティブセッションの統計は
   セッション終了時（`SlotGuard` デストラクタ内）に `accumulated_stats_` へ加算されるため、
   接続中のセッションが送受信したデータは `stats()` に即時反映されない。
   セッションが閉じると反映される。短命接続が多い用途では実用上問題ない。
   リアルタイム反映が必須な場合は `active_connections_` コンテナの追加を検討すること
   （ただし大量接続時の `stats()` 呼び出しコスト増大を伴う）。

7. **`PipeStats` は値型**: コピー・ムーブを提供する（`= default`）。
   Python 側では `stats()` が呼ばれるたびに新しい `PyPipeStats` オブジェクトを生成する。
