param(
    [Parameter(Mandatory=$true)][string]$ReleasePath,
    [string]$BoardIP = "192.168.2.99",
    [string]$User = "xilinx",
    [switch]$AllowLiveHardware
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$release = (Resolve-Path $ReleasePath).Path
python "$root\scripts\release_manifest.py" verify --root "$root" --output "$release"
if ($LASTEXITCODE -ne 0) { throw "Release verification failed; nothing deployed." }
if (-not $AllowLiveHardware) {
    Write-Host "Release verified offline. Deployment requires -AllowLiveHardware."
    exit 0
}
$id = (Get-FileHash "$release\manifest.json" -Algorithm SHA256).Hash.ToLowerInvariant().Substring(0,16)
$stage = "/home/$User/staging/plink-$id"
ssh "${User}@${BoardIP}" "mkdir -p '$stage/release'"
if ($LASTEXITCODE -ne 0) { throw "SSH staging failed" }
scp "$release\manifest.json" "$release\t9_programmer.bit" "$release\t9_programmer.hwh" "$release\t9_programmer_pcap.bin" "${User}@${BoardIP}:$stage/release/"
if ($LASTEXITCODE -ne 0) { throw "Artifact upload failed" }
foreach ($directory in @("scripts", "t9_programmer", "vivado", "constraints", "plink-tray", "hardware")) {
    scp -r "$root\$directory" "${User}@${BoardIP}:$stage/"
    if ($LASTEXITCODE -ne 0) { throw "Source upload failed: $directory" }
}
# Interactive sudo, never a hard-coded password. No USB/IP/hub actions.
ssh -t "${User}@${BoardIP}" "cd '$stage' && sudo bash t9_programmer/deploy.sh --allow-live-hardware"
if ($LASTEXITCODE -ne 0) { throw "Deployment failed; inspect board journal before retrying." }
