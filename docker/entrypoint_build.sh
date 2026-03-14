#!/usr/bin/env bash
# docker/entrypoint_build.sh
# ─────────────────────────────────────────────────────────────────────────────
# コンテナ内で全 Python バージョン分の wheel をビルドし /project/dist/ に出力する。
#
# 環境変数:
#   PY_VERSIONS  (省略可) ビルドするバージョンをスペース区切りで指定
#                例: PY_VERSIONS="3.8 3.11 3.14"
#                省略時は 3.8, 3.9, 3.10, 3.11, 3.12, 3.13, 3.14 全て
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

PY_VERSIONS="${PY_VERSIONS:-3.8 3.9 3.10 3.11 3.12 3.13 3.14}"
DIST_DIR="/project/dist"
mkdir -p "${DIST_DIR}"

# manylinux 内の Python はディレクトリ名が cp38-cp38 / cp310-cp310 / cp313-cp313 等
declare -A PY_EXEC=(
    ["3.8"]="/opt/python/cp38-cp38/bin/python"
    ["3.9"]="/opt/python/cp39-cp39/bin/python"
    ["3.10"]="/opt/python/cp310-cp310/bin/python"
    ["3.11"]="/opt/python/cp311-cp311/bin/python"
    ["3.12"]="/opt/python/cp312-cp312/bin/python"
    ["3.13"]="/opt/python/cp313-cp313/bin/python"
    ["3.14"]="/opt/python/cp314-cp314/bin/python"
)

echo "=== pipeutil Linux wheel builder ==="
echo "    Versions: ${PY_VERSIONS}"
echo ""

FAILED=()

for pyver in ${PY_VERSIONS}; do
    pyexec="${PY_EXEC[${pyver}]:-}"
    if [[ -z "${pyexec}" || ! -x "${pyexec}" ]]; then
        echo "[WARN] Python ${pyver} not found at expected path, skipping."
        FAILED+=("${pyver}")
        continue
    fi

    echo "──────────────────────────────────────────────────────────────"
    echo "  Building wheel for Python ${pyver} ..."
    echo "──────────────────────────────────────────────────────────────"

    # pip / build deps を更新
    "${pyexec}" -m pip install --quiet --upgrade pip build scikit-build-core cmake

    # wheel ビルド（一時ディレクトリを使って dist_dir だけ固定）
    TMPWHL=$(mktemp -d)
    "${pyexec}" -m pip wheel /project -w "${TMPWHL}" --no-deps

    # auditwheel で manylinux タグを付与して dist へ収録
    "${pyexec}" -m pip install --quiet auditwheel
    "${pyexec}" -m auditwheel repair "${TMPWHL}"/pipeutil-*.whl \
        --plat manylinux_2_28_x86_64 \
        --wheel-dir "${DIST_DIR}"

    rm -rf "${TMPWHL}"
    echo "  Done."
done

echo ""
echo "=== Build complete ==="
ls -1 "${DIST_DIR}"/*.whl

if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "[ERROR] Failed versions: ${FAILED[*]}"
    exit 1
fi
