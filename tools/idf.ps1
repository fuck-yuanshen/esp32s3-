param(
    [Parameter(Position = 0)]
    [ValidateSet('build', 'reconfigure', 'menuconfig', 'flash', 'monitor', 'size')]
    [string]$Action = 'build',
    [string]$Port = '',
    [string]$IdfPath = '',
    [string]$ToolsPath = $env:IDF_TOOLS_PATH,
    [string]$PythonEnvPath = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

# Use the SDK selected by this project before considering the inherited shell SDK.
if (-not $IdfPath) {
    $settingsPath = Join-Path $projectRoot '.vscode/settings.json'
    if (Test-Path -LiteralPath $settingsPath) {
        $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
        $IdfPath = $settings.'idf.currentSetup'
    }
    if (-not $IdfPath) { $IdfPath = $env:IDF_PATH }
}
if (-not $IdfPath -or -not (Test-Path -LiteralPath (Join-Path $IdfPath 'tools/idf.py'))) {
    throw 'Set -IdfPath to an installed ESP-IDF directory.'
}
if (-not $ToolsPath -or -not (Test-Path -LiteralPath $ToolsPath)) {
    throw 'Set -ToolsPath to the Espressif tools directory.'
}

# Prefer the matching managed Python venv instead of the system Python version.
if (-not $PythonEnvPath) {
    $versionFile = Join-Path $IdfPath 'tools/cmake/version.cmake'
    $versionText = Get-Content -LiteralPath $versionFile -Raw
    $major = [regex]::Match($versionText, 'set\(IDF_VERSION_MAJOR\s+(\d+)\)').Groups[1].Value
    $minor = [regex]::Match($versionText, 'set\(IDF_VERSION_MINOR\s+(\d+)\)').Groups[1].Value
    $envRoot = Join-Path $ToolsPath 'python_env'
    $candidates = @(Get-ChildItem -LiteralPath $envRoot -Directory |
        Where-Object { $_.Name -like "idf$major.$minor`_py*_env" } |
        Sort-Object Name -Descending)
    if ($candidates.Count -gt 0) { $PythonEnvPath = $candidates[0].FullName }
}
if (-not $PythonEnvPath) {
    throw 'Set -PythonEnvPath to the matching ESP-IDF Python environment.'
}
$pythonExe = Join-Path $PythonEnvPath 'Scripts/python.exe'
if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw 'Set -PythonEnvPath to the matching ESP-IDF Python environment.'
}

$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = $PythonEnvPath
$env:PYTHONUTF8 = '1'
$env:PYTHONNOUSERSITE = 'True'
$env:PATH = (Split-Path -Parent $pythonExe) + ';' + $env:PATH
# activate.py reports progress on stderr; with ErrorActionPreference = Stop,
# Windows PowerShell turns that into a terminating NativeCommandError.
$activationPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
. (Join-Path $IdfPath 'export.ps1')
$ErrorActionPreference = $activationPreference
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'ESP-IDF environment activation did not expose cmake.'
}

# Avoid dependence on a global compiler-cache directory when building the project.
$idfArguments = @('--no-ccache', '-C', $projectRoot)
if ($Port) { $idfArguments += @('-p', $Port) }
$idfArguments += $Action
# idf.py reports Kconfig notices and progress on stderr; keep them as output so
# Windows PowerShell does not turn them into a terminating error.
$ErrorActionPreference = 'Continue'
& $pythonExe (Join-Path $IdfPath 'tools/idf.py') @idfArguments
$idfExitCode = $LASTEXITCODE
$ErrorActionPreference = $activationPreference
exit $idfExitCode
