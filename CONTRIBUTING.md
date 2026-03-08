# コントリビューションガイド

## 目次

1. [環境構築](#1-環境構築)
2. [C++ テストの実行](#2-c-テストの実行)
3. [Python テストの実行](#3-python-テストの実行)
4. [新機能を追加するときのテスト手順](#4-新機能を追加するときのテスト手順)
5. [テストを書くときの規則](#5-テストを書くときの規則)
6. [PR を出す前のチェックリスト](#6-pr-を出す前のチェックリスト)
7. [CI で何が動くか](#7-ci-で何が動くか)

---

## 1. 環境構築

**必要なもの**
| ツール | バージョン | 備考 |
|---|---|---|
| CMake | ≥ 3.25 | |
| Visual Studio | 2022 (17) | Windows のみ |
| GCC / Clang | C++20 対応 | Linux のみ |
| Python | 3.8 - 3.14 | |
| Git | 任意 | |

**リポジトリのクローン**
```powershell
git clone https://github.com/edamame-edame/pipeutil.git
cd pipeutil
```

---

## 2. C++ テストの実行

```powershell
# 構成（vs-test プリセット: Debug + PIPEUTIL_BUILD_TESTS=ON）
cmake --preset vs-test

# ビルド
cmake --build --preset build-test

# テスト実行
ctest --preset run-test
```

`ctest --preset run-test --output-on-failure` で失敗時の詳細出力が得られます。

**Ubuntu の場合**
```bash
cmake -B build/linux-test -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPIPEUTIL_BUILD_TESTS=ON \
    -DPIPEUTIL_BUILD_PYTHON=OFF
cmake --build build/linux-test
ctest --test-dir build/linux-test --output-on-failure
```

---

## 3. Python テストの実行

```powershell
# セットアップ（初回のみ）
python -m venv .venv
.venv\Scripts\Activate.ps1
pip install -e ".[test]"   # pipeutil + pytest + pytest-timeout

# テスト実行
pytest tests/python/ -v
```

**特定テストのみ実行**
```powershell
pytest tests/python/test_timeout.py -v -k "rx_timeout"
```

**タイムアウトを変えて実行する場合**
```powershell
pytest tests/python/ --timeout=60
```

---

## 4. 新機能を追加するときのテスト手順

> **ルール: 機能の実装コードと同じ PR にテストを含めること。テストなしの実装 PR は原則マージしません。**

### ステップ 1 — C++ テストを書く

`tests/cpp/` に新しいテストファイルを追加するか、既存ファイルを拡張します。

```
tests/cpp/
  test_message.cpp      ← Message クラス用
  test_io_buffer.cpp    ← IOBuffer 用
  test_error.cpp        ← PipeErrorCode / PipeException 用
  test_roundtrip.cpp    ← send/receive 統合テスト用
  test_<新機能名>.cpp   ← ← 新規追加
```

`tests/cpp/CMakeLists.txt` に以下を追記します:

```cmake
pipeutil_add_test(test_<新機能名>  test_<新機能名>.cpp)
```

### ステップ 2 — Python テストを書く

`tests/python/` にテストを追加します。

```
tests/python/
  test_message.py
  test_roundtrip.py
  test_timeout.py
  test_<新機能名>.py   ← ← 新規追加
```

### ステップ 3 — ローカルで全テストを確認する

```powershell
# C++
cmake --preset vs-test
cmake --build --preset build-test
ctest --preset run-test

# Python
pytest tests/python/ -v
```

すべて PASS したら PR を作成します。

---

## 5. テストを書くときの規則

### C++ (Google Test)

| 事項 | ルール |
|---|---|
| テスト Suite 名 | `<クラス名>Test` または `<機能>Test` |
| テスト名 | `<条件>_<期待結果>` 形式（例: `DefaultConstruct_IsEmpty`）|
| パイプ名 | `unique_pipe("suffix")` ヘルパーで固有名を生成する |
| タイムアウト設定 | 3000 ms を標準とし、大容量転送は 10000 ms まで許容 |
| `EXPECT_*` vs `ASSERT_*` | 後続ステップが前提に依存する場合は `ASSERT_*` を使う |

### Python (pytest)

| 事項 | ルール |
|---|---|
| ファイル名 | `test_<機能名>.py` |
| クラス名 | `Test<シナリオ名>` |
| 関数名 | `test_<条件>_<期待結果>` |
| フィクスチャ | `conftest.py` の `make_server` を活用する |
| タイムアウト | `receive(timeout_ms)` には必ず有限値を指定する |
| 並列安全性 | `unique_pipe()` でテストごとに固有パイプ名を生成する |

### 境界値テストで必ず確認するポイント

- [ ] タイムアウト（短すぎる / ちょうど / 0 = 無限）
- [ ] 空ペイロード（0 バイト）
- [ ] NULL バイトを含むペイロード
- [ ] 1 バイト / 64 KiB / 1 MiB の大容量
- [ ] 接続前・切断後に I/O 操作したとき
- [ ] 相手側がクローズしたとき（`ConnectionReset` / `BrokenPipe`）

---

## 6. PR を出す前のチェックリスト

```
[ ] ctest --preset run-test がすべて PASS
[ ] pytest tests/python/ がすべて PASS
[ ] 今回の変更に対応した新規テストを追加した
[ ] バグ修正の場合: 回帰テストを追加した
[ ] PR テンプレートのテストチェックリストを記入した
```

---

## 7. CI で何が動くか

CI は **PR 高速系（`ci.yml`）** と **夜間フルマトリクス（`nightly.yml`）** の 2 系統で運用します。

### PR 高速系（`ci.yml`）

push / PR をトリガーに自動実行されます。**圧縮マトリクス**でフィードバックを高速化します。

| Job | OS | Python | 内容 |
|---|---|---|---|
| `cpp-tests` | Windows / Linux | — | CMake Debug → CTest |
| `python-tests` | Windows | 3.8, 3.14 | wheel ビルド → pytest |
| `python-tests` | Linux | 3.8, 3.11, 3.14 | wheel ビルド → pytest |

CI が赤のままの PR はマージできません。

### 夜間フルマトリクス（`nightly.yml`）

毎日 UTC 02:00 に自動実行されます。**全サポートバージョンの互換性を網羅検証**します。
`workflow_dispatch` で手動実行も可能です。

| Job | OS | Python | 内容 |
|---|---|---|---|
| `cpp-tests` | Windows / Linux | — | CMake Debug → CTest |
| `python-tests` | Windows / Linux | 3.8, 3.11, 3.13, 3.14 | wheel ビルド → pytest |

### 使い分け方針

- PR マージ判断: PR 高速系がすべて green であること
- リリース前: `nightly.yml` を手動実行（`workflow_dispatch`）してフルマトリクスを確認
- 中間バージョン固有の不具合: nightly の失敗通知で検出

---

## 補足: テスト追加を忘れないための仕組み

このプロジェクトには以下の仕組みが組み込まれています:

| 仕組み | 場所 | 効果 |
|---|---|---|
| PR テンプレート | `.github/PULL_REQUEST_TEMPLATE.md` | テストチェックリストが PR 本文に自動挿入される |
| Issue テンプレート | `.github/ISSUE_TEMPLATE/feature_request.yml` | 機能提案時にテスト計画の記載を必須にする |
| GitHub Actions CI | `.github/workflows/ci.yml` | テストが壊れている PR は自動的にブロックされる |
| このドキュメント | `CONTRIBUTING.md` | 手順と規則を一箇所に集約 |
