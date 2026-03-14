# F-008 詳細設計: Windows セキュリティ記述子 ACL

**作成日**: 2026-03-11
**対象バージョン**: v0.5.0
**依存**: F-001（MultiPipeServer）, F-002（プロトコルフレーム）

---

## 1. 現状と課題

### 1.1 問題

`PipeServer` は `CreateNamedPipeW` の `lpSecurityAttributes` に `nullptr` を渡しており、
OS デフォルトの ACL（パイプ作成者のみアクセス可）でパイプを作成している。
異なるアカウント（例: Windows サービスアカウント、SYSTEM、管理者など）が接続するシナリオで
`ERROR_ACCESS_DENIED` が発生する。

具体的なユースケース:
- Windows サービス（LocalSystem）がサーバー、ログオンユーザーがクライアント
- 管理者プロセスがサーバー、非特権プロセスがクライアント
- 複数ユーザーセッション（Remote Desktop など）を横断する通信

### 1.2 スコープ定義

| 項目 | 対応 |
|---|---|
| `PipeServer` コンストラクタへの `PipeAcl` パラメータ追加 | ✅ 必須 |
| `MultiPipeServer` コンストラクタへの `PipeAcl` パラメータ追加 | ✅ 必須 |
| Windows: `ConvertStringSecurityDescriptorToSecurityDescriptorW` による SDDL 解析 | ✅ 必須 |
| Linux: UNIX ドメインソケットの `chmod` によるパーミッション制御 | ✅ 必須 |
| `PipeAcl::Default` による後方互換保証 | ✅ 必須（破壊的変更なし） |
| `PipeAcl::Custom` による任意 SDDL 指定 | ✅ 必須（Windows のみ有効） |
| Python バインディング（`PipeAcl` 定数） | ✅ 必須 |
| `PipeClient` 側の ACL 変更 | ❌ 範囲外（クライアントは ACL を設定しない） |
| ACL の実行時変更（`SetNamedPipeHandleState` 等） | ❌ 範囲外 |
| Linux での POSIX ACL（`setfacl`） | ❌ 範囲外（chmod のみ） |

---

## 2. アーキテクチャ概観

```
source/core/include/pipeutil/
  pipe_acl.hpp              ← 新規: PipeAcl enum class
  pipeutil.hpp              ← 更新: pipe_acl.hpp を include 追加
  pipe_server.hpp           ← 更新: コンストラクタに acl / custom_sddl 追加
  multi_pipe_server.hpp     ← 更新: コンストラクタに acl / custom_sddl 追加
  detail/
    platform_pipe.hpp       ← 更新: server_create のシグネチャ拡張

source/core/src/
  pipe_server.cpp           ← 更新: Impl に acl_ / custom_sddl_ フィールド追加
  multi_pipe_server.cpp     ← 更新: server_create 呼び出しに引数追加
  platform/
    win32_pipe.hpp          ← 更新: acl_ / custom_sddl_ フィールド追加
    win32_pipe.cpp          ← 更新: SDDL → SECURITY_ATTRIBUTES 変換実装
    posix_pipe.hpp          ← 更新: sock_mode_ フィールド追加
    posix_pipe.cpp          ← 更新: bind 後に chmod 実装

source/python/
  py_pipe_server.cpp        ← 更新: PipeAcl パラメータをバインディングに追加
  py_multi_pipe_server.cpp  ← 更新: 同様

python/pipeutil/
  __init__.pyi              ← 更新: PipeAcl 型スタブ追加

tests/cpp/
  test_security_acl.cpp     ← 新規: ACL 関連テスト
  CMakeLists.txt            ← 更新: test_security_acl 追加
```

---

## 3. API 設計

### 3.1 `PipeAcl` enum class

```cpp
// source/core/include/pipeutil/pipe_acl.hpp

enum class PipeAcl {
    Default,      // OS デフォルト（nullptr SECURITY_ATTRIBUTES、後方互換）
    LocalSystem,  // SYSTEM + Builtin Admins + インタラクティブユーザー
    Everyone,     // ローカルマシン上の全ユーザー（注意: セキュリティリスク）
    Custom,       // custom_sddl で SDDL 文字列を直接指定（Windows のみ有効）
};
```

#### `PipeAcl` 値と対応 SDDL（Windows）

| 値 | SDDL 文字列 | 説明 |
|---|---|---|
| `Default` | `nullptr` | OS デフォルト ACL |
| `LocalSystem` | `"D:(A;;GA;;;SY)(A;;GRGW;;;BA)(A;;GRGW;;;IU)"` | SYSTEM: 全権, 管理者: 読み書き, IU: 読み書き |
| `Everyone` | `"D:(A;;GA;;;WD)"` | WD (World/Everyone): 全権 |
| `Custom` | 受け取った文字列をそのまま使用 | 任意の SDDL |

#### `PipeAcl` 値と対応パーミッション（Linux）

| 値 | chmod モード | 説明 |
|---|---|---|
| `Default` | 変更なし（umask 適用後の値） | OS デフォルト |
| `LocalSystem` | `0600` (S_IRUSR\|S_IWUSR) | ファイルオーナーのみ（サービスアカウント想定） |
| `Everyone` | `0666` (S_IRUSR\|S_IWUSR\|S_IRGRP\|S_IWGRP\|S_IROTH\|S_IWOTH) | 全ユーザー読み書き |
| `Custom` | → `InvalidArgument` 例外送出 | Linux では SDDL 非対応 |

### 3.2 `PipeServer` コンストラクタ拡張

```cpp
// 後方互換: 既存コードは変更不要（デフォルト引数）
explicit PipeServer(std::string pipe_name,
                    std::size_t buffer_size  = 65536,
                    PipeAcl     acl          = PipeAcl::Default,
                    std::string custom_sddl  = "");
```

### 3.3 `MultiPipeServer` コンストラクタ拡張

```cpp
// 後方互換: 既存コードは変更不要（デフォルト引数）
explicit MultiPipeServer(std::string pipe_name,
                         std::size_t max_connections = 8,
                         std::size_t buffer_size     = 65536,
                         PipeAcl     acl             = PipeAcl::Default,
                         std::string custom_sddl     = "");
```

### 3.4 `IPlatformPipe::server_create` シグネチャ変更

```cpp
// server_create に acl / custom_sddl を追加。
// Win32Pipe / PosixPipe がそれぞれ保持し、server_accept_and_fork でも同一 ACL を再適用する。
virtual void server_create(const std::string& pipe_name,
                           PipeAcl acl,
                           const std::string& custom_sddl) = 0;
```

### 3.5 Python バインディング

```python
import pipeutil

# PipeAcl 定数（列挙値は整数として公開）
pipeutil.PipeAcl.Default      # 0
pipeutil.PipeAcl.LocalSystem  # 1
pipeutil.PipeAcl.Everyone     # 2
pipeutil.PipeAcl.Custom       # 3

# 使用例
server = pipeutil.PipeServer(
    "my_pipe",
    buffer_size=65536,
    acl=pipeutil.PipeAcl.Everyone  # ローカル全ユーザーから接続可
)
```

---

## 4. 実装方針

### 4.1 Windows 実装 (`win32_pipe.cpp`)

```
server_create(pipe_name, acl, custom_sddl)
  ├─ acl == Default  → lpSecurityAttributes = nullptr（現行と同じ）
  ├─ acl == LocalSystem / Everyone
  │     → ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl_str, ...)
  │     → SECURITY_ATTRIBUTES sa = { sizeof(sa), psd, FALSE }
  │     → CreateNamedPipeW(..., &sa)
  │     → LocalFree(psd)  ← 必ず実行（例外安全: RAII ラッパー使用）
  └─ acl == Custom
        → custom_sddl が空なら InvalidArgument 例外
        → utf8_to_wstring(custom_sddl) で UTF-16 変換
        → 同上
```

#### RAII による `PSECURITY_DESCRIPTOR` 管理

```cpp
// スタック上の RAII ラッパーで常に LocalFree を保証する
struct SdRaii {
    PSECURITY_DESCRIPTOR ptr = nullptr;
    ~SdRaii() { if (ptr) LocalFree(ptr); }
};
```

#### `server_accept_and_fork` での ACL 再適用

`Win32Pipe::server_accept_and_fork` では `CreateNamedPipeW` を再呼び出して  
次の接続待ちインスタンスを作成する。同じ SDDL を再適用するため、  
`Win32Pipe` メンバーに `acl_` / `custom_sddl_` を保持し、  
`server_accept_and_fork` 内の `CreateNamedPipeW` 呼び出しにも同一のロジックを適用する。

#### 必要なヘッダー・ライブラリ

```cpp
#include <sddl.h>        // ConvertStringSecurityDescriptorToSecurityDescriptorW
// リンク: Advapi32.lib（CMakeLists.txt で target_link_libraries に追加）
```

### 4.2 Linux 実装 (`posix_pipe.cpp`)

```
server_create(pipe_name, acl, custom_sddl)
  ├─ acl == Custom → InvalidArgument 例外（Linux では SDDL 非対応）
  ├─ bind() 実行（既存処理）
  └─ acl != Default → chmod(sock_path_, モード)
                         Default    : なし（umask のまま）
                         LocalSystem: 0600
                         Everyone   : 0666
```

`PosixPipe` は `sock_mode_` フィールドを持たず、`server_create` 内でローカル変数で完結させる。

### 4.3 後方互換性

- 既存の `PipeServer(name)` / `PipeServer(name, buf)` 呼び出しはすべてそのまま動く
- `PipeAcl::Default` 時は `CreateNamedPipeW` への引数が `nullptr` のままなので、  
  Windows の動作は一切変わらない
- Linux の `Default` 時は `chmod` を実行しないため、既存の umask 適用後のパーミッションを維持

---

## 5. エラー処理

| 状況 | 例外 |
|---|---|
| `PipeAcl::Custom` で `custom_sddl` が空文字列（Windows） | `PipeException(InvalidArgument, "Custom SDDL string is empty")` |
| `PipeAcl::Custom` で SDDL が不正な形式（Windows） | `PipeException(AccessDenied, "Failed to parse SDDL security descriptor")` |
| `PipeAcl::Custom` on Linux | `PipeException(InvalidArgument, "PipeAcl::Custom is not supported on Linux; use Default/LocalSystem/Everyone")` |
| `chmod` 失敗（Linux） | `PipeException(SystemError, "chmod failed on socket file")` |

---

## 6. テスト計画

### 6.1 C++ テスト (`tests/cpp/test_security_acl.cpp`)

| # | テスト名 | 内容 |
|---|---|---|
| 1 | `DefaultAclServerListen` | `PipeAcl::Default` でリッスン成功 |
| 2 | `DefaultAclSameUserConnect` | `Default` でクライアント接続成功 |
| 3 | `LocalSystemAclServerListen` | `LocalSystem` でリッスン成功 |
| 4 | `EveryoneAclServerListen` | `Everyone` でリッスン成功（Win32/Linux 共通） |
| 5 | `CustomAclValidSddlWin32` | 有効な SDDL 文字列でリッスン成功（Windows のみ） |
| 6 | `CustomAclEmptySddlThrows` | 空 SDDL で `InvalidArgument` 例外（Windows） |
| 7 | `CustomAclInvalidSddlThrows` | 不正 SDDL で `AccessDenied` 例外（Windows） |
| 8 | `CustomAclLinuxThrows` | Linux で `Custom` を指定すると `InvalidArgument` 例外 |
| 9 | `MultiPipeServerDefaultAcl` | `MultiPipeServer` の `Default` でリッスン・接続成功 |
| 10 | `MultiPipeServerEveryoneAcl` | `MultiPipeServer` の `Everyone` でリッスン成功 |
| 11 | `AclDoesNotChangeClientAPI` | `PipeClient` 側 API に変更なし（既存テストが通ること） |

---

## 7. 変更ファイルサマリ

| ファイル | 変更種別 | 主な変更内容 |
|---|---|---|
| `spec/F008_security_acl.md` | 新規 | 本仕様書 |
| `source/core/include/pipeutil/pipe_acl.hpp` | 新規 | `PipeAcl` enum class |
| `source/core/include/pipeutil/pipeutil.hpp` | 変更 | `pipe_acl.hpp` include 追加 |
| `source/core/include/pipeutil/detail/platform_pipe.hpp` | 変更 | `server_create` シグネチャ拡張 |
| `source/core/include/pipeutil/pipe_server.hpp` | 変更 | `#include pipe_acl.hpp`、コンストラクタ拡張 |
| `source/core/include/pipeutil/multi_pipe_server.hpp` | 変更 | `#include pipe_acl.hpp`、コンストラクタ拡張 |
| `source/core/src/pipe_server.cpp` | 変更 | `Impl` に `acl_`/`custom_sddl_` 追加 |
| `source/core/src/multi_pipe_server.cpp` | 変更 | `server_create` 引数追加 |
| `source/core/src/platform/win32_pipe.hpp` | 変更 | `acl_`/`custom_sddl_` フィールド追加 |
| `source/core/src/platform/win32_pipe.cpp` | 変更 | SDDL ACL 実装、`#include <sddl.h>` |
| `source/core/src/platform/posix_pipe.hpp` | 変更 | シグネチャ変更 |
| `source/core/src/platform/posix_pipe.cpp` | 変更 | `chmod` パーミッション制御 |
| `source/python/py_pipe_server.cpp` | 変更 | `acl`, `custom_sddl` バインディング追加 |
| `source/python/py_multi_pipe_server.cpp` | 変更 | 同様 |
| `python/pipeutil/__init__.pyi` | 変更 | `PipeAcl` 型スタブ追加 |
| `tests/cpp/test_security_acl.cpp` | 新規 | ACL テスト 11 件 |
| `tests/cpp/CMakeLists.txt` | 変更 | `test_security_acl` ターゲット追加 |
