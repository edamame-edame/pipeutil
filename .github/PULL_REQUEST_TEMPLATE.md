## Summary

<!-- Describe what this PR changes in 1–3 sentences. -->

## Type of Change

- [ ] Bug fix
- [ ] New feature (`docs/feature_proposals_v0.2.md` proposal number: F-__ )
- [ ] Refactoring
- [ ] Documentation
- [ ] CI / build configuration
- [ ] Other

## Related Issue / Ticket

Closes #

---

## ✅ Test Checklist (Required)

> **PRs without tests added or updated will not be merged.**
> Complete all checks below before opening the PR.

### C++ Tests (`tests/cpp/`)

- [ ] All existing tests pass (`ctest --preset run-test`)
- [ ] **Added tests covering this change**, or explained why existing tests are sufficient

  > Test file / test name added:  
  > (e.g.) `tests/cpp/test_message.cpp` → `MessageTest/LargePayload_64KiB`

### Python Tests (`tests/python/`)

- [ ] All existing tests pass (`pytest tests/python/`)
- [ ] **Added pytest covering this change**, or explained why existing tests are sufficient

  > Test file / function name added:  
  > (e.g.) `tests/python/test_roundtrip.py` → `TestBasicRoundTrip::test_echo_server`

### Boundary & Regression Tests

- [ ] Added at least one regression test for any bug fix
- [ ] Considered boundary conditions: timeouts, disconnects, NULL bytes, etc.

---

## Verification

| Environment | Method | Result |
|---|---|---|
| Windows / MSVC | `ctest --preset run-test` | ✅ / ❌ |
| Windows / Python | `pytest tests/python/` | ✅ / ❌ |

---

## Review Focus

<!-- Specific areas you want the reviewer to check -->

---

## Review-Informed Checklist

- [ ] Public API, type stubs, docstrings, README, and sample code signatures are consistent
- [ ] If Python C extension was touched: ownership, GIL, return value checks, and export visibility were verified
- [ ] If frame format or payload constraints were touched: tables, diagrams, structs, and boundary tests were updated together
- [ ] If Windows/POSIX shutdown paths or I/O completion was touched: `close`/`shutdown`/`poll`/overlapped completion races were verified
- [ ] If metrics or timeouts were touched: accumulation contract, reset races, and finite-timeout regression were verified
