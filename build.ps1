$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
# 两个 cpp 编译成一个程序，通过 chat.h 共用类型和接口声明。
# -municode 对应 ui.cpp 的 wmain，用于正确接收中文命令行参数。
$options = @('-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-O0', '-g', '-municode', '-pthread', '-static', '-finput-charset=UTF-8', '-fexec-charset=UTF-8')
g++ @options ui.cpp chat.cpp -o chat.exe -lws2_32
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
Write-Host 'Done. Run .\chat.exe'
