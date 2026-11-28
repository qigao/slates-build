param([switch]$ForConfigure)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dotenv = Join-Path $root '.env'
$pathKeys = @('SALTS_BASE_ROOT', 'SALTS_UTILS_ROOT', 'CMETA_ROOT', 'LLVM_MINGW_ROOT')
$keys = $pathKeys + @('SALTS_BASE_VERSION', 'SALTS_UTILS_VERSION', 'CMETA_VERSION', 'CMAKE_BUILD_PARALLEL_LEVEL')
$assignment = '^\s*(' + ($keys -join '|') + ')\s*=(.*)$'
$fileValues = @{}

if (Test-Path -LiteralPath $dotenv) {
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $dotenv -Encoding UTF8) {
        $lineNumber++
        if ($line -cnotmatch $assignment) {
            continue
        }
        $key = $Matches[1]
        $value = $Matches[2].Trim()
        if ($fileValues.ContainsKey($key)) {
            throw "Duplicate .env key $key on line $lineNumber."
        }
        if ($value -match '^["'']') {
            if ($value -notmatch '^(["''])(.*)\1$') {
                throw "Unterminated .env value for $key on line $lineNumber."
            }
            $value = $Matches[2]
        }
        $fileValues[$key] = $value
    }
}

foreach ($key in $keys) {
    $value = [Environment]::GetEnvironmentVariable($key, 'Process')
    if ($null -eq $value) {
        $value = $fileValues[$key]
    }
    if ($key -in $pathKeys) {
        if ([string]::IsNullOrWhiteSpace($value)) {
            if ($ForConfigure) {
                throw "Set $key in the parent environment or in $dotenv."
            }
            continue
        }
        if (-not [IO.Path]::IsPathRooted($value)) {
            $value = Join-Path $root $value
        }
        if (-not (Test-Path -LiteralPath $value -PathType Container)) {
            throw "$key must point to a directory: $value"
        }
        $value = (Resolve-Path -LiteralPath $value).Path
    }
    if ($null -ne $value) {
        [Environment]::SetEnvironmentVariable($key, $value, 'Process')
    }
}

if (-not $ForConfigure) {
    return
}

$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
$triple = switch ($architecture.ToString()) {
    'X64'   { 'x86_64-w64-mingw32' }
    'Arm64' { 'aarch64-w64-mingw32' }
    default { throw "LLVM-MinGW bootstrap requires a Windows x64 or ARM64 host, got $architecture." }
}
$compiler = Join-Path $env:LLVM_MINGW_ROOT "bin/${triple}-clang.exe"
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "LLVM-MinGW compiler not found: $compiler"
}
return $compiler
