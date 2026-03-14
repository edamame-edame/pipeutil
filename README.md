# pipeutil

High-speed IPC pipe communication library for Windows/Linux with a C++ core and Python bindings.

## Supported Platforms

- C++: C++20
- Python: 3.8 - 3.14
- OS: Windows, Linux
- Release artifacts:
	- Windows wheel: cp38-cp314 `win_amd64`
	- Linux wheel: cp38-cp314 `manylinux_2_27_x86_64.manylinux_2_28_x86_64`

## Release Verification

`v1.0.0` is release-verified on all supported Python versions.

- Windows: Python 3.8-3.14, each version `110 passed, 9 skipped`
- Linux: Python 3.8-3.14, each version `113 passed, 6 skipped`
- Linux wheels are built and tested through Docker-based manylinux workflows in [docker-compose.yml](docker-compose.yml) and [docker](docker).

For the compressed release review summary, see [review/whole.md](review/whole.md).

## Agent Roles

- Main implementer: Claude
- Reviewer role: GPT-family reviewers such as Code X and GPT-5.4
- Review outputs: [review/whole.md](review/whole.md) and the latest aggregated CSV under [review](review)

Implementation guidance is maintained in [CLAUDE.md](CLAUDE.md). Reviewer guidance is maintained in [CODEX.md](CODEX.md).

## CI Strategy

This project uses a two-tier CI strategy:

- **PR Fast Checks** (`.github/workflows/ci.yml`)
	- Trigger: push / pull_request
	- Python matrix: Windows (3.8, 3.14), Linux (3.8, 3.11, 3.14)
	- Goal: fast feedback for day-to-day development

- **Nightly Full Matrix** (`.github/workflows/nightly.yml`)
	- Trigger: daily schedule (UTC 02:00) and `workflow_dispatch`
	- Python matrix: Windows/Linux × 3.8, 3.11, 3.13, 3.14
	- Goal: full compatibility coverage across supported versions

Before release, run `nightly.yml` manually with `workflow_dispatch` and confirm full-matrix green.

Also run the Linux Docker release flow and confirm all wheels import and test successfully across cp38-cp314.

For detailed CI operation rules and merge criteria, see [CONTRIBUTING.md §7](CONTRIBUTING.md#7-what-runs-in-ci).
