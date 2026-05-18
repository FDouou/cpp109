# clean_logs.ps1 — 删除测试/benchmark 产生的所有 .log 文件
# 用法: .\clean_logs.ps1

$root = Split-Path -Parent $PSCommandPath
Get-ChildItem -Path $root -Recurse -Include "*.log" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Write-Host "All .log files removed."
