# Contributing Guide

## Table of Contents

1. [Environment Setup](#1-environment-setup)
2. [Running C++ Tests](#2-running-c-tests)
3. [Running Python Tests](#3-running-python-tests)
4. [Testing Workflow for New Features](#4-testing-workflow-for-new-features)
5. [Test Writing Conventions](#5-test-writing-conventions)
6. [Pre-PR Checklist](#6-pre-pr-checklist)
7. [What Runs in CI](#7-what-runs-in-ci)

---

## 1. Environment Setup

**Prerequisites**
| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.25 | |
| Visual Studio | 2022 (17) | Windows only |
| GCC / Clang | C++20-compatible | Linux only |
| Python | 3.8 - 3.14 | |
| Git | any | |

**Clone the repository**
```powershell
git clone https://github.com/edamame-edame/pipeutil.git
cd pipeutil
```

---

## 2. Running C++ Tests

```powershell
# Configure (vs-test preset: Debug + PIPEUTIL_BUILD_TESTS=ON)
cmake --preset vs-test

# Build
cmake --build --preset build-test

# Run tests
ctest --preset run-test
```

Use `ctest --preset run-test --output-on-failure` to see detailed output on failure.

**On Ubuntu**
```bash
cmake -B build/linux-test -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPIPEUTIL_BUILD_TESTS=ON \
    -DPIPEUTIL_BUILD_PYTHON=OFF
cmake --build build/linux-test
ctest --test-dir build/linux-test --output-on-failure
```

---

## 3. Running Python Tests

```powershell
# First-time setup
python -m venv .venv
.venv\Scripts\Activate.ps1
pip install -e ".[test]"   # pipeutil + pytest + pytest-timeout

# Run tests
pytest tests/python/ -v
```

**Run a specific test**
```powershell
pytest tests/python/test_timeout.py -v -k "rx_timeout"
```

**Run with a custom timeout**
```powershell
pytest tests/python/ --timeout=60
```

---

## 4. Testing Workflow for New Features

> **Rule: every implementation PR must include tests. PRs without tests will not be merged.**

### Step 1 — Write C++ tests

Add a new test file under `tests/cpp/`, or extend an existing one.

```
tests/cpp/
  test_message.cpp      ← for the Message class
  test_io_buffer.cpp    ← for IOBuffer
  test_error.cpp        ← for PipeErrorCode / PipeException
  test_roundtrip.cpp    ← send/receive integration tests
  test_<feature>.cpp    ← add new file here
```

Register it in `tests/cpp/CMakeLists.txt`:

```cmake
pipeutil_add_test(test_<feature>  test_<feature>.cpp)
```

### Step 2 — Write Python tests

Add tests under `tests/python/`.

```
tests/python/
  test_message.py
  test_roundtrip.py
  test_timeout.py
  test_<feature>.py     ← add new file here
```

### Step 3 — Verify all tests pass locally

```powershell
# C++
cmake --preset vs-test
cmake --build --preset build-test
ctest --preset run-test

# Python
pytest tests/python/ -v
```

Once all tests pass, open a PR.

---

## 5. Test Writing Conventions

### C++ (Google Test)

| Item | Convention |
|---|---|
| Test suite name | `<ClassName>Test` or `<Feature>Test` |
| Test name | `<Condition>_<ExpectedResult>` (e.g. `DefaultConstruct_IsEmpty`) |
| Pipe name | Generate unique names using the `unique_pipe("suffix")` helper |
| Timeout | 3000 ms as standard; up to 10000 ms for large-payload transfers |
| `EXPECT_*` vs `ASSERT_*` | Use `ASSERT_*` when later steps depend on a precondition |

### Python (pytest)

| Item | Convention |
|---|---|
| File name | `test_<feature>.py` |
| Class name | `Test<ScenarioName>` |
| Function name | `test_<condition>_<expected_result>` |
| Fixtures | Use `make_server` from `conftest.py` |
| Timeout | Always pass a finite value to `receive(timeout_ms)` |
| Isolation | Use `unique_pipe()` to generate a unique pipe name per test |

### Boundary conditions to always verify

- [ ] Timeout (too short / exact / 0 = infinite)
- [ ] Empty payload (0 bytes)
- [ ] Payload containing NULL bytes
- [ ] 1 byte / 64 KiB / 1 MiB large payloads
- [ ] I/O operations before connect or after disconnect
- [ ] Peer-side close (`ConnectionReset` / `BrokenPipe`)

---

## 6. Pre-PR Checklist

```
[ ] ctest --preset run-test — all passed
[ ] pytest tests/python/   — all passed
[ ] Added new tests covering this change
[ ] For bug fixes: added a regression test
[ ] Filled in the test checklist in the PR template
```

---

## 7. What Runs in CI

CI runs two pipelines: **PR fast checks (`ci.yml`)** and **nightly full matrix (`nightly.yml`)**.

### PR Fast Checks (`ci.yml`)

Triggered automatically on push / PR with a **compressed matrix** for fast feedback.

| Job | OS | Python | Description |
|---|---|---|---|
| `cpp-tests` | Windows / Linux | — | CMake Debug → CTest |
| `python-tests` | Windows | 3.8, 3.14 | build wheel → pytest |
| `python-tests` | Linux | 3.8, 3.11, 3.14 | build wheel → pytest |

PRs with a failing CI cannot be merged.

### Nightly Full Matrix (`nightly.yml`)

Runs automatically at UTC 02:00 every day. **Covers full compatibility across all supported versions.**
Can also be triggered manually via `workflow_dispatch`.

| Job | OS | Python | Description |
|---|---|---|---|
| `cpp-tests` | Windows / Linux | — | CMake Debug → CTest |
| `python-tests` | Windows / Linux | 3.8, 3.11, 3.13, 3.14 | build wheel → pytest |

### When to use which

- Merge decision: all PR fast checks must be green
- Before release: run `nightly.yml` manually (`workflow_dispatch`) to confirm the full matrix
- Version-specific regressions: detected via nightly failure notifications

---

## Note: Safeguards to Prevent Missing Tests

The following mechanisms are built into this project:

| Mechanism | Location | Effect |
|---|---|---|
| PR template | `.github/PULL_REQUEST_TEMPLATE.md` | Test checklist is auto-inserted into every PR body |
| Issue template | `.github/ISSUE_TEMPLATE/feature_request.yml` | Requires a test plan for feature proposals |
| GitHub Actions CI | `.github/workflows/ci.yml` | PRs that break tests are blocked automatically |
| This document | `CONTRIBUTING.md` | Centralizes procedures and conventions |
