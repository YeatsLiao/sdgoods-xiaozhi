# 谷仓次元屏 xiaozhi 固件构建脚本（本机 ESP-IDF 5.5.5 + py3.11 venv）
# 用法: powershell -ExecutionPolicy Bypass -File tools\build.ps1
$env:IDF_TOOLS_PATH     = 'D:\1.Soft\Espressif'
$env:IDF_PATH           = 'D:\1.Soft\Espressif\frameworks\esp-idf-v5.5.5'
$env:IDF_PYTHON_ENV_PATH= 'D:\1.Soft\Espressif\python_env\idf5.5_py3.11_env'
$env:PYTHONIOENCODING   = 'utf-8'   # 避免 export 子进程输出 GBK 解码崩溃

. "$env:IDF_PATH\export.ps1"
Set-Location (Split-Path $PSScriptRoot -Parent)
idf.py -B build_s3 build
if ($LASTEXITCODE -eq 0) { idf.py -B build_s3 merge-bin }
Write-Host "BUILD_EXIT=$LASTEXITCODE"
exit $LASTEXITCODE
