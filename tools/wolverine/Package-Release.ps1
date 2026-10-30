param([Parameter(Mandatory)][string]$BuildDirectory,
      [Parameter(Mandatory)][string]$OutputDirectory,
      [Parameter(Mandatory)][ValidatePattern('^v[0-9][0-9A-Za-z.+-]*$')][string]$Version)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot)
$BuildDirectory=(Resolve-Path -LiteralPath $BuildDirectory).Path
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$name="KytyPS5-Wolverine-$Version-Windows-x64"
$stage=Join-Path $OutputDirectory $name
if(Test-Path -LiteralPath $stage){throw 'Use a fresh output directory; package already exists.'}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
cmake --install $BuildDirectory --prefix $stage
if($LASTEXITCODE){throw 'CMake install failed'}
foreach($file in 'Start-Wolverine.ps1','Play Wolverine.bat') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
$licenses=Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
foreach($entry in Get-ChildItem -LiteralPath (Join-Path $root 'LICENSES')) {
    Copy-Item -LiteralPath $entry.FullName -Destination $licenses -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $root '3rdparty/winpthread/COPYING') -Destination (Join-Path $licenses 'winpthread-COPYING.txt')
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $stage 'LEIA-ME.md')
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination (Join-Path $stage 'docs') -Recurse
Copy-Item -LiteralPath (Join-Path $root 'research') -Destination (Join-Path $stage 'research') -Recurse
$revision=(& git -C $root rev-parse HEAD).Trim()
if($LASTEXITCODE){throw 'Cannot determine source revision'}
$hash=(Get-FileHash -LiteralPath (Join-Path $stage 'kyty_emulator.exe') -Algorithm SHA256).Hash
@{version=$Version;source_commit=$revision;repository='https://github.com/calang000/KytyPS5-Wolverine';
  status='experimental';sha256=$hash;task_runtime=1;write_fault_cluster=2} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'build.json') -Encoding utf8
$excluded=@(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Name -match '^(eboot\.bin|param\.(json|sfo|sfx))$' -or $_.Extension -in '.pkg','.pdb','.tracy','.etl','.srt-replay' -or
    $_.FullName -match '[\\/](_SaveData|_PipelineCache|app0)[\\/]'
})
if($excluded.Count){throw 'Unexpected game, save, cache or diagnostic content in package'}
$zip=Join-Path $OutputDirectory ($name+'.zip')
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$zipHash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$zipHash  $([IO.Path]::GetFileName($zip))" | Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding ascii
Write-Output $zip
