## 変更概要

<!-- このプルリクエストで何を変更したか、1〜3 文で説明してください。 -->

## 変更種別

- [ ] バグ修正
- [ ] 新機能（`docs/feature_proposals_v0.2.md` の提案番号: F-__ ）
- [ ] リファクタリング
- [ ] ドキュメント
- [ ] CI / ビルド設定
- [ ] その他

## 関連 Issue / チケット

Closes #

---

## ✅ テストチェックリスト（必須）

> **テストを追加・更新しないマージは原則禁止です。**
> 以下のチェックをすべて記入してから PR をオープンしてください。

### C++ テスト（`tests/cpp/`）

- [ ] 既存テストがすべて通過している（`ctest --preset run-test`）
- [ ] **今回の変更に対応したテストを追加した** または 既存テストで網羅されている理由を説明した

  > 追加したテストファイル / テスト名:  
  > （例）`tests/cpp/test_message.cpp` → `MessageTest/LargePayload_64KiB`

### Python テスト（`tests/python/`）

- [ ] 既存テストがすべて通過している（`pytest tests/python/`）
- [ ] **今回の変更に対応した pytest を追加した** または 既存テストで網羅されている理由を説明した

  > 追加したテストファイル / 関数名:  
  > （例）`tests/python/test_roundtrip.py` → `TestBasicRoundTrip::test_echo_server`

### 境界値・回帰テスト

- [ ] 修正したバグには最低 1 件の回帰テストを追加した
- [ ] タイムアウト・断切れ・NULL バイトなど境界条件を考慮した

---

## 動作確認

| 環境 | 確認方法 | 結果 |
|---|---|---|
| Windows / MSVC | `ctest --preset run-test` | ✅ / ❌ |
| Windows / Python | `pytest tests/python/` | ✅ / ❌ |

---

## レビュー観点

<!-- レビュアーに特に確認してほしい点を記載 -->
