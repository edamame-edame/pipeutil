#!/usr/bin/env bash
# docker/entrypoint_test.sh
# ─────────────────────────────────────────────────────────────────────────────
# コンテナ内で全 Python バージョン分の pytest を実行する。
# dist/ に対応 wheel がある場合はそれをインストールし、
# ない場合はソースから直接インストールしてテストを行う。
#
# 環境変数:
#   PY_VERSIONS  (省略可) テストするバージョンをスペース区切りで指定
#   PYTEST_ARGS  (省略可) pytest に追加で渡す引数
#                例: PYTEST_ARGS="-x -k roundtrip"
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

PY_VERSIONS="${PY_VERSIONS:-3.8 3.9 3.10 3.11 3.12 3.13 3.14}"
PYTEST_ARGS="${PYTEST_ARGS:--v --tb=short --timeout=30}"
DIST_DIR="/project/dist"

declare -A PY_EXEC=(
    ["3.8"]="/opt/python/cp38-cp38/bin/python"
    ["3.9"]="/opt/python/cp39-cp39/bin/python"
    ["3.10"]="/opt/python/cp310-cp310/bin/python"
    ["3.11"]="/opt/python/cp311-cp311/bin/python"
    ["3.12"]="/opt/python/cp312-cp312/bin/python"
    ["3.13"]="/opt/python/cp313-cp313/bin/python"
    ["3.14"]="/opt/python/cp314-cp314/bin/python"
)

echo "=== pipeutil Linux test runner ==="
echo "    Versions  : ${PY_VERSIONS}"
echo "    pytest arg: ${PYTEST_ARGS}"
echo ""

FAILED=()
PASSED=()

for pyver in ${PY_VERSIONS}; do
    pyexec="${PY_EXEC[${pyver}]:-}"
    if [[ -z "${pyexec}" || ! -x "${pyexec}" ]]; then
        echo "[WARN] Python ${pyver} not found, skipping."
        FAILED+=("${pyver}")
        continue
    fi

    echo "══════════════════════════════════════════════════════════════"
    echo "  Testing Python ${pyver}"
    echo "══════════════════════════════════════════════════════════════"

    # cp タグ例: 3.14 → cp314
    cptag="cp$(echo "${pyver}" | tr -d '.')"

    # 対応 wheel を探す（manylinux 優先、次に raw linux）
    whl=$(ls "${DIST_DIR}"/*${cptag}*manylinux*.whl 2>/dev/null | head -1 || true)
    if [[ -z "${whl}" ]]; then
        whl=$(ls "${DIST_DIR}"/*${cptag}*linux*.whl 2>/dev/null | head -1 || true)
    fi

    if [[ -n "${whl}" ]]; then
        echo "  Installing ${whl##*/} ..."
        "${pyexec}" -m pip install --quiet --force-reinstall "${whl}"
    else
        echo "  No prebuilt wheel found. Installing from source ..."
        "${pyexec}" -m pip install --quiet \
            build scikit-build-core cmake
        "${pyexec}" -m pip install --quiet /project
    fi

    # テスト依存インストール
    "${pyexec}" -m pip install --quiet \
        "pytest>=8.0" "pytest-timeout>=2.3" \
        "pytest-asyncio>=0.23" "msgpack>=1.0"

    # pytest 実行
    set +e
    "${pyexec}" -m pytest /project/tests/python/ ${PYTEST_ARGS}
    ret=$?
    set -e

    if [[ ${ret} -eq 0 ]]; then
        PASSED+=("${pyver}")
        echo "  [PASS] Python ${pyver}"
    else
        FAILED+=("${pyver}")
        echo "  [FAIL] Python ${pyver} (exit ${ret})"
    fi
    echo ""
done

echo "══════════════════════════════════════════════════════════════"
echo "  Results"
echo "══════════════════════════════════════════════════════════════"
[[ ${#PASSED[@]} -gt 0 ]] && echo "  PASS: ${PASSED[*]}"
[[ ${#FAILED[@]} -gt 0 ]] && echo "  FAIL: ${FAILED[*]}"

if [[ ${#FAILED[@]} -gt 0 ]]; then
    exit 1
fi
