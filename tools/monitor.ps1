$env:IDF_TOOLS_PATH='D:\1.Soft\Espressif'
$env:IDF_PATH='D:\1.Soft\Espressif\frameworks\esp-idf-v5.5.5'
$env:IDF_PYTHON_ENV_PATH='D:\1.Soft\Espressif\python_env\idf5.5_py3.11_env'
$env:PYTHONIOENCODING='utf-8'
. "$env:IDF_PATH\export.ps1" *> $null
Set-Location (Split-Path $PSScriptRoot -Parent)
idf.py -B build_s3 -p COM6 monitor
