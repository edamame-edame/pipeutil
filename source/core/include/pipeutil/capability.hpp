// capability.hpp — Capability Negotiation 公開型 (v1.1.0)
// 仕様: spec/A001_capability_negotiation.md
#pragma once

#include "pipeutil_export.hpp"
#include <chrono>
#include <cstdint>

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// Capability — feature_bitmap の各ビットに対応する機能識別子 (A-001 §3)
// ──────────────────────────────────────────────────────────────────────────────
enum class Capability : uint32_t {
    ProtoV2        = 0x01u,  // 24-byte v2 ヘッダ対応（F-009/F-010/F-012/F-013 全完了後）
    ConcurrentRpc  = 0x02u,  // 並行 RPC / in-flight 複数リクエスト（F-009, v1.3.0）
    Streaming      = 0x04u,  // ストリーミングモード（F-010, v1.4.0）
    Heartbeat      = 0x08u,  // ハートビート / プロセス監視（F-012, v1.3.0）
    PriorityQueue  = 0x10u,  // 優先度キュー（F-013, v1.4.0）
    // bits 6-31 = reserved（将来拡張。送信時は 0 とすること）
};

// ──────────────────────────────────────────────────────────────────────────────
// NegotiatedCapabilities — HELLO 交換後に得られる合意済み機能セット
// （client_bitmap & server_bitmap の結果）(A-001 §4)
// ──────────────────────────────────────────────────────────────────────────────
struct PIPEUTIL_API NegotiatedCapabilities {
    uint32_t bitmap    = 0u;    // 合意済み機能の OR（client_bitmap & server_bitmap）
    bool     v1_compat = false; // true: 接続相手が v1.0.0 クライアント（version=0x01 で接続）

    /// 指定した機能が合意されているか確認する
    [[nodiscard]] bool has(Capability cap) const noexcept {
        return (bitmap & static_cast<uint32_t>(cap)) != 0u;
    }

    /// 何も合意されていない（v1 モードにフォールバックした）か。
    /// v1-compat（v1.0.0 クライアントが接続）の場合は false を返す。
    /// v1.1.0 同士で HELLO 交換したが双方 bitmap=0 の場合に true を返す。
    [[nodiscard]] bool is_legacy_v1() const noexcept { return bitmap == 0u && !v1_compat; }

    /// v1.0.0 クライアントとの v1-compat 接続かどうかを返す。
    /// is_legacy_v1() == true でも、v1-compat（旧クライアント）と
    /// フォールバック（v1.1.0 クライアントが HELLO 未送信）を区別できる。
    [[nodiscard]] bool is_v1_compat() const noexcept { return v1_compat; }
};

// ──────────────────────────────────────────────────────────────────────────────
// HelloMode — 接続ごとの HELLO ハンドシェイクポリシー (A-001 §4.1)
// ──────────────────────────────────────────────────────────────────────────────
enum class HelloMode {
    /// (デフォルト) v1.0.0 クライアントを受け入れ v1-compat モードで処理する。
    /// v1.1.0 クライアントから HELLO が届かない場合は v1 フォールバック（例外なし）。
    /// Rolling upgrade: サーバー先行アップグレード期間の推奨設定。
    Compat,

    /// HELLO なしで接続してきた v1.1.0 クライアントや v1.0.0 クライアントを拒否する。
    /// hello_timeout 内に HELLO が届かなければ ConnectionRejected を送出する。
    /// 全内部ネットワークなど v1.1.0 専用環境で推奨。
    Strict,

    /// HELLO ハンドシェイクをスキップし、即座に v1 モードで動作する。
    /// 接続待機オーバーヘッドを最小化したい場合（テスト・ベンチマーク等）に使用。
    /// **両側が Skip を設定した場合のみ使用すること。**
    Skip,
};

// ──────────────────────────────────────────────────────────────────────────────
// HelloConfig — HELLO ハンドシェイクの動作設定 (A-001 §4.1)
// ──────────────────────────────────────────────────────────────────────────────
struct PIPEUTIL_API HelloConfig {
    /// ハンドシェイクポリシー。デフォルトは Compat（Rolling upgrade 対応）。
    HelloMode mode = HelloMode::Compat;

    /// サーバー側の HELLO 受信タイムアウト（ms）。
    /// version=0x02 クライアントからの先頭 5B 読み取りを待機する時間。
    /// 0 を指定すると無制限待機。version=0x01 判別は即座のためこの値は適用外。
    std::chrono::milliseconds hello_timeout{500};

    /// 自身が対応する capability ビットの OR。
    /// v1.1.0 では未実装機能のビットは 0 のまま。各機能実装時（v1.3.0〜）に更新する。
    uint32_t advertised_capabilities = 0u;
};

}  // namespace pipeutil
