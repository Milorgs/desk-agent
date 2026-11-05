param([string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\desk-agent'), [switch]$NoPath)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'desk.exe'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw 'Keep install.ps1 beside desk.exe in the extracted package.' }
$target = Join-Path $Prefix 'desk.exe'
$marker = Join-Path $Prefix 'desk-agent-installed'
if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath $marker)) { throw 'Another desk.exe exists here. Choose a different -Prefix.' }
New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
$temp = Join-Path $Prefix ('.desk-install-' + [Guid]::NewGuid().ToString('N') + '.exe')
try { Copy-Item -LiteralPath $source -Destination $temp; Move-Item -LiteralPath $temp -Destination $target -Force } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force } }
Set-Content -LiteralPath $marker -Value 'desk-agent installation' -Encoding ASCII
if (-not $NoPath) {
  $current = [Environment]::GetEnvironmentVariable('Path', 'User')
  $parts = @($current -split ';' | Where-Object { $_ })
  if (-not ($parts | Where-Object { $_.TrimEnd('\') -ieq $Prefix.TrimEnd('\') })) {
    [Environment]::SetEnvironmentVariable('Path', (($parts + $Prefix) -join ';'), 'User')
  }
}
Write-Host "Installed: $target"
Write-Host 'Open a new terminal and run desk. No model settings or API keys are included in this package.'
