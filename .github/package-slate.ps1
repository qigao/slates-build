param([Parameter(Mandatory)][string]$Platform)

$ErrorActionPreference = 'Stop'
$suffix = if ($IsWindows) { '.exe' } else { '' }
$version = & "./out/ci/slate$suffix" --version
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^slate (\d+\.\d+\.\d+)$') {
    throw "Invalid Slate version: $version"
}
$name = "slate-$($Matches[1])-$Platform"
$output = (New-Item -ItemType Directory -Path out/packages -Force).FullName
$staging = (New-Item -ItemType Directory -Path out/package-stage -Force).FullName
$prefix = Join-Path $staging $name
cmake --install out/ci --prefix $prefix
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Push-Location $staging
try {
    if ($IsWindows) {
        $archive = Join-Path $output "$name.zip"
        cmake -E tar cf $archive --format=zip $name
    } else {
        $archive = Join-Path $output "$name.tar.gz"
        cmake -E tar czf $archive --format=gnutar $name
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

$relocated = (New-Item -ItemType Directory -Path 'out/package verification' -Force).FullName
Push-Location $relocated
try {
    cmake -E tar xf $archive
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

$project = Join-Path $relocated 'package-check'
$presets = (New-Item -ItemType Directory -Path (Join-Path $project 'presets') -Force).FullName
Copy-Item -LiteralPath 'apps/presets/NativeToolchains.json','apps/presets/DefaultPreset.json' -Destination $presets
Set-Content -LiteralPath (Join-Path $project 'slate.json') -Encoding utf8 -Value @'
{
  "include": ["presets/NativeToolchains.json", "presets/DefaultPreset.json"],
  "defaultPreset": "debug",
  "modules": ["."]
}
'@
Set-Content -LiteralPath (Join-Path $project 'module.json') -Encoding utf8 -Value @'
{"kind":"executable","sources":{"files":["main.c"]}}
'@
Set-Content -LiteralPath (Join-Path $project 'main.c') -Encoding utf8 -Value @'
#include <stdio.h>
int main(void) {
  puts("Hello from slate.");
  return 0;
}
'@

$sdkStage = (Resolve-Path (Join-Path $env:RUNNER_TEMP 'slate-sdks')).Path
$hidden = @()
try {
    foreach ($sdk in @('SALTS_BASE_ROOT', 'SALTS_UTILS_ROOT', 'CMETA_ROOT')) {
        $directory = (Resolve-Path (Join-Path $sdkStage $sdk)).Path
        if ((Split-Path $directory -Parent) -ne $sdkStage) { throw "Unexpected SDK path: $directory" }
        Rename-Item -LiteralPath $directory -NewName "$sdk.hidden"
        $hidden += $sdk
        Remove-Item -LiteralPath "Env:$sdk" -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath Env:LD_LIBRARY_PATH,Env:DYLD_LIBRARY_PATH -ErrorAction SilentlyContinue
    $binary = Join-Path $relocated "$name/bin/slate$suffix"
    $arguments = @('run', '--root', $project, '--target', 'package-check', '--build-dir', 'out/ci-package')
    if ($IsWindows) {
        $env:LLVM_MINGW_ROOT = Split-Path (Split-Path $env:CC -Parent) -Parent
        $arguments += '--compiler', 'llvm-mingw'
    }
    $result = & $binary @arguments
    $status = $LASTEXITCODE
    $result
    if ($status -ne 0 -or $result -notcontains 'Hello from slate.') {
        throw "The relocated Slate archive could not build and run the package check."
    }
} finally {
    foreach ($sdk in $hidden) {
        Rename-Item -LiteralPath (Join-Path $sdkStage "$sdk.hidden") -NewName $sdk
    }
}
