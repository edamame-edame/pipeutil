# pipeutil

High-speed IPC pipe communication library for Windows/Linux with a C++ core and Python bindings.

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
