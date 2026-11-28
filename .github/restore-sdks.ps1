param([Parameter(Mandatory)][string]$Profile)

$ErrorActionPreference = 'Stop'
$lock = Get-Content (Join-Path $PSScriptRoot 'ci-inputs.json') -Raw | ConvertFrom-Json -AsHashtable
if (-not $lock.ContainsKey($Profile)) {
    throw "Unknown SDK profile $Profile."
}

$staging = Join-Path $env:RUNNER_TEMP 'slate-sdks'
$curl = (Get-Command curl -CommandType Application | Select-Object -First 1).Source
foreach ($asset in $lock[$Profile]) {
    $destination = Join-Path $staging $asset.env
    New-Item -Path $destination -ItemType Directory -Force | Out-Null
    $archive = Join-Path $staging ([IO.Path]::GetFileName($asset.url))
    & $curl --fail --location --retry 3 --output $archive $asset.url
    if ($LASTEXITCODE -ne 0) { throw "Download failed for $($asset.url)." }
    if ((Get-FileHash -LiteralPath $archive).Hash -ne $asset.sha256) {
        throw "SHA-256 mismatch for $archive."
    }

    tar -xf $archive -C $destination
    if ($LASTEXITCODE -ne 0) { throw "Could not extract $archive." }
    $configFiles = @(Get-ChildItem -LiteralPath $destination -File -Recurse -Filter $asset.marker)
    if ($configFiles.Count -ne 1) { throw "Expected one $($asset.marker) in $archive." }
    $value = $configFiles[0].DirectoryName
    if ($asset.env -eq 'CC') {
        Add-Content -LiteralPath $env:GITHUB_PATH -Value $value -Encoding utf8
        $value = $configFiles[0].FullName
    }
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "$($asset.env)=$value" -Encoding utf8
    if ($asset.env -in @('SALTS_BASE_ROOT', 'SALTS_UTILS_ROOT', 'CMETA_ROOT')) {
        if ($asset.url -notmatch '/releases/download/v([0-9]+\.[0-9]+\.[0-9]+)/') {
            throw "Expected an exact SDK release in $($asset.url)."
        }
        $versionKey = $asset.env -replace '_ROOT$', '_VERSION'
        Add-Content -LiteralPath $env:GITHUB_ENV -Value "$versionKey=$($Matches[1])" -Encoding utf8
    }
}
