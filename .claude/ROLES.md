# Agent Role Map

このディレクトリは、Claude 系エージェントに対してプロジェクト内の役割分担を明示するための補助メモです。

## Primary Roles

- Claude: 主実装者
- GPT 系モデル（Code X / GPT-5.4 など）: レビュアー

## Operational Rules

- 実装、テスト、レビュー反映は主に Claude が担当する。
- 最終レビュー判断、修正確認、新規リスク検出は GPT 系レビュアーが担当する。
- 実装前に Claude は [../CLAUDE.md](../CLAUDE.md) を読み、レビュー後は [../review/whole.md](../review/whole.md) を確認する。
- レビュアーは [../CODEX.md](../CODEX.md) と [../AGENTS.md](../AGENTS.md) のルールに従う。

## Records

- review 配下は最新の集約 CSV と whole.md のみを保持する。
- 恒久ルールは README、CLAUDE、AGENTS、.github 配下へ順次移送する。