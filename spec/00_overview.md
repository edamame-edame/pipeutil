# pipeutil — プロジェクト概要仕様

## 1. プロジェクト目的

`pipeutil` は、異なるプロセス間（IPC）で高速なパイプ通信を行うためのクロスプラットフォームライブラリです。
C++ ネイティブコードをコアとして実装し、Python C API ラッパー経由で Python からも利用可能にします。

### 対応する通信パターン

| 送信側  | 受信側  | 備考 |
|---------|---------|------|
| C++     | C++     | ネイティブ利用（ヘッダ + DLL/SO） |
| C++     | Python  | C++ が Server / Python が Client、またはその逆 |
| Python  | Python  | ラッパー経由で C++ コアを利用 |

---

## 2. 設計方針

| 項目 | 方針 |
|------|------|
| 言語標準 | C++20 / Python 3.13 |
| バイナリ形式 | Windows: `.dll` + `.lib`、Linux: `.so` |
| Python バインディング | Python C API 拡張モジュール（`_pipeutil.pyd` / `_pipeutil.so`） |
| 通信トランスポート | Windows: 名前付きパイプ (`\\.\pipe\<name>`)、Linux: UNIX ドメインソケット (`/tmp/pipeutil/<name>.sock`) |
| スレッドモデル | スレッドセーフ（ミューテックス保護）、非同期オプション（`std::future` / select/epoll） |
| エラー処理 | C++: 例外中心（`PipeException` / `std::system_error`）、Python: 例外変換 |
| バッファ管理 | ゼロコピー志向、固定長ヘッダ + 可変長ペイロード |

---

## 3. システムアーキテクチャ

```
┌─────────────────────────────────────────────┐
│           アプリケーション層                  │
│  ┌──────────────┐    ┌──────────────────┐   │
│  │  C++ App     │    │   Python App     │   │
│  └──────┬───────┘    └────────┬─────────┘   │
│         │                     │              │
│         │ include/link        │ import       │
│         ▼                     ▼              │
│  ┌──────────────┐    ┌──────────────────┐   │
│  │ pipeutil.hpp │    │  pipeutil (pkg)  │   │
│  │  (C++ API)   │    │  __init__.py     │   │
│  └──────┬───────┘    └────────┬─────────┘   │
│         │                     │              │
│         │                     │ Python C API │
│         │              ┌──────▼──────────┐  │
│         │              │  _pipeutil      │  │
│         │              │  (.pyd / .so)   │  │
│         │              └──────┬──────────┘  │
│         │                     │ dlopen/     │
│         └──────────┬──────────┘ LoadLibrary │
│                    ▼                         │
│         ┌──────────────────────┐            │
│         │  pipeutil_core       │            │
│         │  (.dll / .so)        │            │
│         │  ┌────────────────┐  │            │
│         │  │  PlatformLayer │  │            │
│         │  │  (Win/Linux)   │  │            │
│         │  └────────────────┘  │            │
│         └──────────────────────┘            │
└─────────────────────────────────────────────┘
                    │ OS IPC
          ┌─────────┴──────────┐
          │  Named Pipe / UDS  │
          └────────────────────┘
```

---

## 4. コンポーネント一覧

### 4.1 `pipeutil_core` — C++ コアライブラリ

**ビルド成果物**: `pipeutil_core.dll` (Windows) / `libpipeutil_core.so` (Linux)

| モジュール | 役割 |
|-----------|------|
| `PlatformPipe` | OS 固有パイプ操作の抽象化 (CRTP or pimpl) |
| `PipeServer` | サーバー側：listen / accept / read / write |
| `PipeClient` | クライアント側：connect / read / write |
| `Message` | フレーミング付きメッセージ構造体 |
| `IOBuffer` | ゼロコピー I/O バッファ管理 |
| `PipeError` | エラー種別列挙 + `std::system_error` ラッパー |

### 4.2 `_pipeutil` — Python C API 拡張モジュール

**ビルド成果物**: `_pipeutil.pyd` (Windows) / `_pipeutil.so` (Linux)

| モジュール | 役割 |
|-----------|------|
| `PyPipeServer` | `PipeServer` の Python 型ラッパー |
| `PyPipeClient` | `PipeClient` の Python 型ラッパー |
| `PyMessage` | `Message` の Python 型ラッパー |
| GIL 管理 | ブロッキング I/O 中に `Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS` |

### 4.3 `pipeutil` — Python パッケージ

**役割**: `_pipeutil` 拡張の使いやすいラッパー、型ヒント、ユーティリティ

---

## 5. ディレクトリ構成（予定）

```
pipeutil/
├── spec/                       # 仕様書 (このディレクトリ)
│   ├── 00_overview.md
│   ├── 01_cpp_core.md
│   ├── 02_protocol.md
│   ├── 03_platform.md
│   ├── 04_python_wrapper.md
│   └── 05_build.md
│
├── source/
│   ├── core/                   # C++ コアライブラリ
│   │   ├── include/
│   │   │   └── pipeutil/
│   │   │       ├── pipeutil.hpp          # 公開 API ヘッダ
│   │   │       ├── pipe_server.hpp
│   │   │       ├── pipe_client.hpp
│   │   │       ├── message.hpp
│   │   │       ├── io_buffer.hpp
│   │   │       └── pipe_error.hpp
│   │   └── src/
│   │       ├── pipe_server.cpp
│   │       ├── pipe_client.cpp
│   │       ├── message.cpp
│   │       ├── io_buffer.cpp
│   │       ├── platform/
│   │       │   ├── win32_pipe.cpp        # Windows 実装
│   │       │   └── posix_pipe.cpp        # Linux 実装
│   │       └── CMakeLists.txt
│   │
│   ├── python/                 # Python C API 拡張
│   │   ├── _pipeutil_module.cpp
│   │   ├── py_pipe_server.cpp
│   │   ├── py_pipe_client.cpp
│   │   ├── py_message.cpp
│   │   └── CMakeLists.txt
│   │
│   └── CMakeLists.txt          # ルート CMake
│
├── python/                     # Python パッケージ
│   └── pipeutil/
│       ├── __init__.py
│       ├── server.py
│       ├── client.py
│       └── py.typed
│
├── tests/
│   ├── cpp/                    # C++ 単体テスト (Google Test)
│   └── python/                 # Python テスト (pytest)
│
├── examples/
│   ├── cpp_to_cpp/
│   ├── cpp_to_python/
│   └── python_to_python/
│
├── review/                     # コードレビュー結果
├── CMakeLists.txt              # トップレベル CMake
└── README.md
```

---

## 6. 非機能要件

| 項目 | 目標値 |
|------|--------|
| スループット（同一マシン）| ≥ 500 MB/s (大容量メッセージ) |
| レイテンシ（往復）| ≤ 100 µs (1 KB メッセージ) |
| 最大メッセージサイズ | 4 GiB − 1 バイト（プロトコル上限: `uint32_t` 最大値 4,294,967,295）。推奨運用上限は 2 GiB |
| スレッドセーフ | Server・Client ともに複数スレッドから安全に利用可能 |
| メモリリーク | Valgrind / AddressSanitizer でクリーン |
| Python GIL | ブロッキング I/O 中は GIL を解放 |

---

## 7. 依存ライブラリ

| ライブラリ | 用途 | 必須 / オプション |
|-----------|------|------------------|
| C++ 標準ライブラリ (C++20) | コア実装 | 必須 |
| `<windows.h>` | Windows パイプ API | Windows 必須 |
| POSIX (`<sys/socket.h>` 等) | Linux UDS | Linux 必須 |
| Python 3.13 ヘッダ (`Python.h`) | Python C API 拡張 | Python バインディング必須 |
| CMake ≥ 3.25 | ビルドシステム | 必須 |
| Google Test | C++ 単体テスト | オプション（開発用） |
| pytest | Python テスト | オプション（開発用） |

---

## 8. 将来拡張（スコープ外）

- マルチクライアント接続プール
- TLS/暗号化トランスポート
- 共有メモリ (mmap) ファストパス
- async/await 対応（Python `asyncio`）
- ブロードキャスト / pub-sub モード
