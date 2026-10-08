# 谷仓次元屏 xiaozhi 固件构建脚本
# 用法:
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1            # 默认 ESP-IDF 6.1（v2.5.0 需要）
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Idf 5.5   # 切回 ESP-IDF 5.5.5（v2.4.0）
param(
    [ValidateSet('6.1', '5.5')]
    [string]$Idf = '6.1'
)

$env:PYTHONIOENCODING = 'utf-8'   # 避免 export 子进程输出 GBK 解码崩溃
$env:PYTHONUTF8        = '1'       # UTF-8 模式：activate_venv.py 读子进程管道不再按 GBK 解码崩溃

if ($Idf -eq '5.5') {
    # 旧工具链：本机 ESP-IDF 5.5.5 + py3.11 venv（对应 backup-pre250 / v2.4.0）
    $env:IDF_TOOLS_PATH      = 'D:\1.Soft\Espressif'
    $env:IDF_PATH            = 'D:\1.Soft\Espressif\frameworks\esp-idf-v5.5.5'
    $env:IDF_PYTHON_ENV_PATH = 'D:\1.Soft\Espressif\python_env\idf5.5_py3.11_env'
} else {
    # 新工具链：EIM 安装的 ESP-IDF 6.1 + py3.14 venv（v2.5.0 要求 IDF >= 6.0.1）
    # Python 环境由 export.ps1 从 IDF_TOOLS_PATH\python_env 自动识别（idf6.1_py3.14_env）。
    $env:IDF_TOOLS_PATH = 'D:\1.Soft\.espressif'
    $env:IDF_PATH       = 'D:\1.Soft\.espressif\v6.1\esp-idf'
}

. "$env:IDF_PATH\export.ps1"
Set-Location (Split-Path $PSScriptRoot -Parent)
idf.py -B build_s3 build
if ($LASTEXITCODE -eq 0) { idf.py -B build_s3 merge-bin }
Write-Host "BUILD_EXIT=$LASTEXITCODE"
exit $LASTEXITCODE
