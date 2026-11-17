$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$toolDir = Join-Path $projectRoot '.cache/tooling/rtk/1.2.0'
New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
$archivePath = Join-Path $toolDir 'rtk.zip'
Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/rtk-ai/rtk/releases/download/v1.2.0/rtk-oss-x86_64-pc-windows-msvc.zip' -OutFile $archivePath
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'd105aaaa55822252f04ac28989e9069c1ea61755ca561940d887cb0b89c785fc') {
    throw 'RTK archive hash mismatch; extraction stopped.'
}
Expand-Archive -LiteralPath $archivePath -DestinationPath $toolDir -Force
$executable = Join-Path $toolDir 'rtk.exe'
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'ede70a7ae605525670d6862d54dbdacbeed383e4a8d81cbcdd0b33b6ef530732') {
    throw 'RTK executable hash mismatch; execution stopped.'
}
& $executable --version
exit $LASTEXITCODE
