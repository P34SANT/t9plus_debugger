# deploy_to_board.ps1 - Transfer and deploy T9+ programmer suite to the board
$BoardIP = "192.168.2.99"
$User = "xilinx"

Write-Host "==> Checking connection to Antminer T9+ at $BoardIP..."
if (-not (Test-Connection -ComputerName $BoardIP -Count 1 -Quiet)) {
    Write-Error "Board at $BoardIP is not reachable!"
    exit 1
}

Write-Host "==> Creating staging directory on $BoardIP..."
ssh -o StrictHostKeyChecking=no "${User}@${BoardIP}" "mkdir -p ~/staging"

Write-Host "==> Copying project files to $BoardIP..."
scp -o StrictHostKeyChecking=no -r t9_programmer_pcap.bin t9_programmer.bit t9_programmer.hwh scripts t9_programmer "${User}@${BoardIP}:~/staging/"

Write-Host "==> Executing deployment on $BoardIP..."
ssh -o StrictHostKeyChecking=no "${User}@${BoardIP}" "cd ~/staging && echo xilinx | sudo -S bash t9_programmer/deploy.sh"

Write-Host "`n==> Verifying running services on $BoardIP..."
ssh -o StrictHostKeyChecking=no "${User}@${BoardIP}" "systemctl status t9-fpga t9-openocd t9-uart-bridge t9-io-daemon --no-pager"
