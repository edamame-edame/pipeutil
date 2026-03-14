<#
.SYNOPSIS
    全サポート Python バージョン (3.8-3.14) の venv を作成し、
    テスト依存パッケージとビルド済み wheel をインストールします。

.DESCRIPTION
    各 Python バージョンに対して以下を実行します:
      1. .venvXX が存在しない場合は venv を新規作成
      2. pip をアップグレード
      3. dist/ に対応 cpXX タグの wheel があればインストール
      4. テスト依存パッケージ (pytest, pytest-asyncio 等) をインストール

.EXAMPLE
    .\tools\setup_venvs.ps1
#>

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$versions = @(
    @{ ver = "3.8";  cp = "cp38";  venv = ".venv38"  },
    @{ ver = "3.9";  cp = "cp39";  venv = ".venv39"  },
    @{ ver = "3.10"; cp = "cp310"; venv = ".venv310" },
    @{ ver = "3.11"; cp = "cp311"; venv = ".venv311" },
    @{ ver = "3.12"; cp = "cp312"; venv = ".venv312" },
    @{ ver = "3.13"; cp = "cp313"; venv = ".venv313" },
    @{ ver = "3.14"; cp = "cp314"; venv = ".venv314" }
)

$failed = @()

foreach ($v in $versions) {
    Write-Host ""
    Write-Host "=== Python $($v.ver) → $($v.venv) ===" -ForegroundColor Cyan

    # ─── 1. venv 作成 ────────────────────────────────────────────────
    if (-not (Test-Path "$($v.venv)\Scripts\python.exe")) {
        Write-Host "  Creating venv ..."
        & py "-$($v.ver)" -m venv $v.venv
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "  [SKIP] Failed to create venv for Python $($v.ver)"
            $failed += $v.ver
            continue
        }
    } else {
        Write-Host "  venv already exists."
    }

    $pip    = "$($v.venv)\Scripts\pip.exe"
    $python = "$($v.venv)\Scripts\python.exe"

    # ─── 2. pip 更新 ─────────────────────────────────────────────────
    & $python -m pip install --quiet --upgrade pip

    # ─── 3. ビルド済み wheel のインストール ─────────────────────────
    $whl = Get-ChildItem "dist\*$($v.cp)*win_amd64.whl" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($whl) {
        Write-Host "  Installing $($whl.Name) ..."
        & $pip install --quiet --force-reinstall $whl.FullName
    } else {
        Write-Warning "  Wheel not found for $($v.cp) — run build first"
    }

    # ─── 4. テスト依存パッケージ ─────────────────────────────────────
    Write-Host "  Installing test deps ..."
    & $pip install --quiet `
        "pytest>=8.0" `
        "pytest-timeout>=2.3" `
        "pytest-asyncio>=0.23" `
        "msgpack>=1.0"

    Write-Host "  OK" -ForegroundColor Green
}

Write-Host ""
if ($failed.Count -eq 0) {
    Write-Host "All venvs ready." -ForegroundColor Green
} else {
    Write-Warning "Some versions failed: $($failed -join ', ')"
    exit 1
}
