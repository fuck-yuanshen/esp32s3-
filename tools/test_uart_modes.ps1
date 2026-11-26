param([string]$VcVarsPath = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build/host_uart_modes'
$include = Join-Path $out 'include'

# Use an installed MSVC toolchain without changing the global shell environment.
if (-not $VcVarsPath) {
    $candidates = @(Get-ChildItem -Path "$env:ProgramFiles/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat" -ErrorAction SilentlyContinue)
    if ($candidates.Count) { $VcVarsPath = $candidates[-1].FullName }
}
if (-not (Test-Path -LiteralPath $VcVarsPath -PathType Leaf)) {
    throw 'Set -VcVarsPath to the vcvars64.bat from an installed Visual C++ toolchain.'
}

# Header shims route only this host build to deterministic platform substitutes.
$headers = @('esp_err.h', 'esp_log.h', 'esp_check.h', 'esp_timer.h', 'sdkconfig.h',
             'driver/uart.h', 'driver/gpio.h', 'freertos/FreeRTOS.h',
             'freertos/semphr.h', 'freertos/task.h')
foreach ($header in $headers) {
    $path = Join-Path $include $header
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value '#include "platform_stub.h"' -Encoding ascii
}
$source = Join-Path $root 'tests/host/test_uart_modes.c'
$paths = @($include, "$root/tests/host", "$root/components/uart_link/include",
           "$root/components/stm32_isp/include", "$root/components/stm32_isp")
$includes = ($paths | ForEach-Object { '/I"' + $_ + '"' }) -join ' '
Push-Location -LiteralPath $out
try {
    foreach ($append in @(1, 0)) {
        # ESP32 size_t is 32-bit; host x64 adds narrowing warnings at SDK calls.
        $command = 'call "{0}" >nul && cl /nologo /std:c11 /W3 /WX /wd4996 /wd4267 /D_CRT_SECURE_NO_WARNINGS /DHOST_APPEND_CRLF={1} {2} "{3}" /Fe:uart_modes_{1}.exe /Fo:uart_modes_{1}.obj && uart_modes_{1}.exe' -f $VcVarsPath, $append, $includes, $source
        & $env:ComSpec /d /s /c $command
        if ($LASTEXITCODE -ne 0) { throw "UART/ISP host checks failed (APPEND_CRLF=$append)." }
    }
    $swdSources = @("$root/tests/host/test_swd.c", "$root/components/stm32_isp/swd_wire.c",
                    "$root/components/stm32_isp/arm_dap.c", "$root/components/stm32_isp/stm32f1_flash.c")
    $quotedSources = ($swdSources | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $command = 'call "{0}" >nul && cl /nologo /std:c11 /W3 /WX {1} {2} /Fe:swd.exe && swd.exe' -f $VcVarsPath, $includes, $quotedSources
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'SWD host checks failed.' }
} finally { Pop-Location }
