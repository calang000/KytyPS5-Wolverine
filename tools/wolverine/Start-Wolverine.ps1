param([string]$GamePath, [string]$ProfilePath, [switch]$PrepareOnly)
$ErrorActionPreference = 'Stop'
$emulator = Join-Path $PSScriptRoot 'kyty_emulator.exe'
if (!(Test-Path -LiteralPath $emulator)) {throw 'kyty_emulator.exe must be beside this launcher.'}
if (!$ProfilePath) {$ProfilePath = Join-Path $env:LOCALAPPDATA 'KytyPS5-Wolverine\profiles\PPSA03671'}
$ProfilePath = [IO.Path]::GetFullPath($ProfilePath)
New-Item -ItemType Directory -Path $ProfilePath -Force | Out-Null
$remembered = Join-Path $ProfilePath 'game-path.txt'
if (!$GamePath -and (Test-Path -LiteralPath $remembered)) {$GamePath = [IO.File]::ReadAllText($remembered).Trim()}
if (!$GamePath) {$GamePath = Read-Host 'Pasta do seu dump, contendo eboot.bin'}
$GamePath = $GamePath.Trim().Trim('"')
$GamePath = (Resolve-Path -LiteralPath $GamePath).Path
if (!(Test-Path -LiteralPath (Join-Path $GamePath 'eboot.bin'))) {throw 'eboot.bin nao encontrado nessa pasta.'}
if (Get-Process kyty_emulator -ErrorAction SilentlyContinue) {throw 'Feche a outra execucao do Kyty antes de iniciar.'}

# Curated settings apply only to this child process, never to the user's global environment.
foreach ($entry in @(Get-ChildItem Env: | Where-Object {$_.Name -like 'KYTY_*'})) {
    [Environment]::SetEnvironmentVariable($entry.Name, $null, 'Process')
}
$settings = @{
    KYTY_SRT_NATIVE=1;KYTY_SRT_COMPILED=1;KYTY_SRT_NATIVE_BATCH=1
    KYTY_REGION_INIT_BITMAP=1;KYTY_SRT_BULK_READS=1;KYTY_SRT_VALUE_ALIASES=1
    KYTY_SRT_CAPTURE_SPANS=2;KYTY_SRT_CANONICAL_VIEWS=1
    KYTY_ASYNC_WRITE_READBACK=1;KYTY_RANGE_NODE_REUSE=1;KYTY_RECORD_THREAD=1
    KYTY_RESOURCE_WORKER=0;KYTY_SRT_DIRECT_READ=0;KYTY_SRT_BUFFER_VIEWS=0;KYTY_RESOURCE_MEMO=0
    KYTY_SRT_BUFFER_GROUP=0;KYTY_WRITE_FAULT_CLUSTER=2
    KYTY_MESH_TASK_INDIRECT=1;KYTY_MESH_TASK_RUNTIME=1;KYTY_MESH_TASK_STATS=0
    KYTY_SHADER_READ_CHUNKS=2;KYTY_PROGRAM_DISK_CACHE=2
    KYTY_PROGRAM_CACHE_GIT_REVISION='3099e882331481450bd06f38e9aeb44d6e6b26bc'
    KYTY_VRAM_SAVER=2;KYTY_VMA_BLOCK_MB=64;KYTY_BINDLESS_EVICT=1
    KYTY_BINDLESS_EVICT_TIGHT_AGE=120;KYTY_MEMORY_GRAVEYARD_MB=2048
    KYTY_FAULT_WATCHED_UNMAPPED=1;KYTY_FAULT_REPAIR_HOST_PAGE=1;KYTY_FAULT_SETTLE_MS=50
    KYTY_IMAGE_RETIRE_FRAMES=0
}
foreach ($entry in $settings.GetEnumerator()) {
    [Environment]::SetEnvironmentVariable($entry.Key, [string]$entry.Value, 'Process')
}
$config = Join-Path $ProfilePath 'config'
New-Item -ItemType Directory -Path $config -Force | Out-Null
$sourceConfig = Join-Path $GamePath 'commandline.txt'
$text = if (Test-Path -LiteralPath $sourceConfig) {[IO.File]::ReadAllText($sourceConfig)} else {''}
$text += "`r`n-perf_spec_ps5_60`r`n{ Shared.PerfSpec=kPS5_60X Shared.OverridePerfSpec=true Render.UseStickyConfig=false }`r`n"
[IO.File]::WriteAllText((Join-Path $config 'commandline.txt'),$text,[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($remembered,$GamePath,[Text.UTF8Encoding]::new($false))
$env:KYTY_APP0_OVERRIDE_DIR=$config
if ($PrepareOnly) {Write-Output "Perfil preparado: $ProfilePath";return}
$logs=Join-Path $ProfilePath 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$log=Join-Path $logs ('wolverine-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'.log')
$skip='bad108e74fb72e9f,4e7f2c6bb9b158a1,8bfd230b9cd875a2,c4df2a00067e0666,dc76e1223a9bf673,e6d76d24f59f8015,4c582eb7319682a3,be2a9db103759bff,d6ec1c4ba883d308,4d2a3a54c74938ad'
Write-Host 'KytyPS5 Wolverine - build EXPERIMENTAL. Nao ha garantia de 30 FPS.'
Write-Host "Saves e caches: $ProfilePath"
Write-Host "Log: $log"
Push-Location -LiteralPath $ProfilePath
try {
    & $emulator --game $GamePath --bindless --redzone --tessellation --readback-linear-images true --skip-shaders $skip *> $log
    exit $LASTEXITCODE
} finally {Pop-Location}
