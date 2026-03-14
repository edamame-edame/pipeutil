// pipe_acl.hpp — パイプサーバーのアクセス制御レベル定義
// 仕様: spec/F008_security_acl.md
#pragma once

#include "pipeutil_export.hpp"

namespace pipeutil {

// ──────────────────────────────────────────────────────────────────────────────
// PipeAcl — PipeServer / MultiPipeServer に適用するアクセス制御レベル
//
// Windows: SDDL（Security Descriptor Definition Language）形式で
//          CreateNamedPipeW の SECURITY_ATTRIBUTES に変換される。
// Linux:   UNIX ドメインソケットのファイルパーミッション（chmod）で制御される。
//          Custom は Linux では非対応（InvalidArgument 例外を送出）。
//
// 後方互換: PipeAcl::Default はコンストラクタのデフォルト引数であり、
//           既存コードを一切変更せずに利用できる。
// ──────────────────────────────────────────────────────────────────────────────
enum class PIPEUTIL_API PipeAcl {
    Default,      ///< OS デフォルト ACL（後方互換。現行動作と同等）
    LocalSystem,  ///< SYSTEM + Builtin Admins + インタラクティブユーザー（Win: SDDL / Linux: chmod 0600）
    Everyone,     ///< ローカルマシン上の全ユーザー（注意: セキュリティリスクあり。Win: SDDL / Linux: chmod 0666）
    Custom,       ///< カスタム SDDL 文字列で指定（Windows のみ有効。Linux では PipeException(InvalidArgument) を送出）
};

} // namespace pipeutil
