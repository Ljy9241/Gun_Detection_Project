param(
    [ValidateRange(1, 65535)]
    [int]$Port = 8000
)
$ErrorActionPreference = 'Stop'
$pythonPath = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $pythonPath)) {
    throw '请先按 cloud_backend/README.md 安装本地运行环境。'
}
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    & $pythonPath -m uvicorn cloud_backend.main:app --host 127.0.0.1 --port $Port
    if ($LASTEXITCODE -ne 0) { throw '接收服务启动失败，请查看上方错误。' }
} finally {
    Pop-Location
}
