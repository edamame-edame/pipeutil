<#
.SYNOPSIS
    指定 Python バージョンの wheel をビルドし、専用 venv にインストールして pytest を実行します。

.PARAMETER PyVersion
    Python バージョン文字列 (例: 3.8, 3.14)

.PARAMETER CpTag
    CPython ABI タグ (例: cp38, cp314)

.PARAMETER VenvDir
    venv ディレクトリ名 (例: .venv38)

.EXAMPLE
    .\tools\build_test.ps1 -PyVersion 3.8 -CpTag cp38 -VenvDir .venv38
#>
param(
    [Parameter(Mandatory)][string]$PyVersion,
    [Parameter(Mandatory)][string]$CpTag,
    [Parameter(Mandatory)][string]$VenvDir
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# ─── 1. wheel ビルド ─────────────────────────────────────────────────
Write-Host ""
Write-Host "=== [py$PyVersion] Build wheel ===" -ForegroundColor Cyan

# 既存の古い wheel を削除してから再ビルド
Remove-Item "dist\*${CpTag}*win_amd64.whl" -ErrorAction SilentlyContinue

& py "-$PyVersion" -m pip wheel . -w dist/
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$whl = Get-ChildItem "dist\*${CpTag}*win_amd64.whl" |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $whl) { Write-Error "Wheel not found for $CpTag after build"; exit 1 }
Write-Host "  Built: $($whl.Name)" -ForegroundColor Green

# ─── 2. venv 作成 ────────────────────────────────────────────────────
Write-Host "=== [py$PyVersion] Setup venv ($VenvDir) ===" -ForegroundColor Cyan
if (-not (Test-Path "$VenvDir\Scripts\python.exe")) {
    & py "-$PyVersion" -m venv $VenvDir
}

$pip    = "$VenvDir\Scripts\pip.exe"
$python = "$VenvDir\Scripts\python.exe"

# ─── 3. wheel + テスト依存 インストール ─────────────────────────────
& $python -m pip install --quiet --upgrade pip
& $pip install --quiet --force-reinstall $whl.FullName
& $pip install --quiet `
    "pytest>=8.0" `
    "pytest-timeout>=2.3" `
    "pytest-asyncio>=0.23" `
    "msgpack>=1.0"

# ─── 4. pytest 実行 ─────────────────────────────────────────────────
Write-Host "=== [py$PyVersion] Run pytest ===" -ForegroundColor Cyan
& $python -m pytest tests/python/ -v --tb=short --timeout=30

exit $LASTEXITCODE
