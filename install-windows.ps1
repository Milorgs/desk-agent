$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = 'Milorgs/desk-agent'
$asset = "https://github.com/$repo/releases/latest/download/desk-agent-windows-x64.zip"
$temp = Join-Path ([IO.Path]::GetTempPath()) ('desk-agent-install-' + [Guid]::NewGuid().ToString('N'))
$zip = Join-Path $temp 'desk-agent.zip'
$package = Join-Path $temp 'package'

try {
    New-Item -ItemType Directory -Path $package -Force | Out-Null
    Invoke-WebRequest -Uri $asset -OutFile $zip -UseBasicParsing
    Expand-Archive -LiteralPath $zip -DestinationPath $package -Force
    # The release ZIP contains desk.exe and install.ps1 at its root.
    $installer = Join-Path $package 'install.ps1'
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw 'Invalid desk-agent package.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installer
    if ($LASTEXITCODE -ne 0) { throw "Installation failed (exit code $LASTEXITCODE)." }
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
