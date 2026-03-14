<#
.SYNOPSIS
    Docker を使って Linux 向け wheel のビルドと pytest を実行します。

.PARAMETER Mode
    実行モード:
      build       - wheel ビルドのみ
      test        - pytest のみ（dist/ に wheel が必要）
      build-test  - ビルド + テスト (default)
      rebuild     - イメージも再ビルド後 build-test

.PARAMETER PyVersions
    対象の Python バージョンをスペース区切りで指定（省略時: 全バージョン）
    例: "3.11 3.14"

.PARAMETER PytestArgs
    pytest に追加で渡す引数（省略時: "-v --tb=short --timeout=30"）

.EXAMPLE
    .\tools\linux_build_test.ps1
    .\tools\linux_build_test.ps1 -Mode build
    .\tools\linux_build_test.ps1 -Mode test -PyVersions "3.11 3.14"
    .\tools\linux_build_test.ps1 -Mode rebuild
#>
param(
    [ValidateSet("build", "test", "build-test", "rebuild")]
    [string]$Mode = "build-test",
    [string]$PyVersions = "",
    [string]$PytestArgs  = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# ─── Docker / docker compose が使えるか確認 ──────────────────────────────
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error "docker コマンドが見つかりません。Docker Desktop をインストールして下さい。"
    exit 1
}

# ─── 環境変数の構築 ──────────────────────────────────────────────────────
$env:PY_VERSIONS = if ($PyVersions) { $PyVersions } else { "3.8 3.9 3.10 3.11 3.12 3.13 3.14" }
$env:PYTEST_ARGS  = if ($PytestArgs)  { $PytestArgs  } else { "-v --tb=short --timeout=30" }

Write-Host ""
Write-Host "=== pipeutil Linux CI (Docker) ===" -ForegroundColor Cyan
Write-Host "  Mode        : $Mode"
Write-Host "  PY_VERSIONS : $env:PY_VERSIONS"
Write-Host "  PYTEST_ARGS : $env:PYTEST_ARGS"
Write-Host ""

# ─── イメージビルド ───────────────────────────────────────────────────────
function Build-Image {
    Write-Host "--- Building Docker image ---" -ForegroundColor Yellow
    docker compose build linux-builder
    if ($LASTEXITCODE -ne 0) { Write-Error "Docker image build failed"; exit 1 }
}

# ─── モード別実行 ────────────────────────────────────────────────────────
switch ($Mode) {
    "rebuild" {
        Build-Image
        Write-Host "--- Build wheels ---" -ForegroundColor Yellow
        docker compose run --rm linux-build
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Host "--- Run pytest ---" -ForegroundColor Yellow
        docker compose run --rm linux-test
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    "build" {
        # 初回はイメージが無いので pull/build する
        docker compose run --rm linux-build
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    "test" {
        docker compose run --rm linux-test
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    "build-test" {
        docker compose run --rm linux-build-test
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green

# 生成された Linux wheel を表示
$linuxWheels = Get-ChildItem "dist\*linux*.whl" -ErrorAction SilentlyContinue
if ($linuxWheels) {
    Write-Host "Linux wheels:"
    $linuxWheels | ForEach-Object { Write-Host "  $($_.Name)" }
}
