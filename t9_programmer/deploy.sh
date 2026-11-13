#!/bin/bash
# ==============================================================================
# deploy.sh - Antminer T9+ STM32 Programmer & Pod Installer
# ==============================================================================
set -e

INSTALL_DIR="/home/xilinx/t9_programmer"
echo "==> Deploying Antminer T9+ Programmer to $INSTALL_DIR..."

sudo mkdir -p "$INSTALL_DIR/bitstream"
sudo mkdir -p "$INSTALL_DIR/scripts"
sudo mkdir -p "$INSTALL_DIR/openocd"
sudo mkdir -p "$INSTALL_DIR/uart_bridge"
sudo mkdir -p "$INSTALL_DIR/io_daemon"
sudo mkdir -p "$INSTALL_DIR/usbip_dap"
sudo mkdir -p "$INSTALL_DIR/bin"
sudo mkdir -p "$INSTALL_DIR/systemd"

# Copy binaries and scripts
sudo cp scripts/t9_load_bit.py "$INSTALL_DIR/scripts/"
sudo chmod +x "$INSTALL_DIR/scripts/t9_load_bit.py"
if [ -f scripts/t9_esp32_flash.py ]; then
    sudo cp scripts/t9_esp32_flash.py "$INSTALL_DIR/scripts/"
    sudo chmod +x "$INSTALL_DIR/scripts/t9_esp32_flash.py"
fi

sudo cp t9_programmer_pcap.bin "$INSTALL_DIR/bitstream/"
sudo cp t9_programmer.bit "$INSTALL_DIR/bitstream/"
sudo cp t9_programmer.hwh "$INSTALL_DIR/bitstream/"

sudo cp -r t9_programmer/usbip_dap/* "$INSTALL_DIR/usbip_dap/"
(cd "$INSTALL_DIR/usbip_dap" && make clean && make)
sudo ln -sfn "$INSTALL_DIR/usbip_dap" /home/xilinx/usbip_dap

sudo cp t9_programmer/openocd/* "$INSTALL_DIR/openocd/"
sudo chmod +x "$INSTALL_DIR/openocd/"*.sh

sudo cp t9_programmer/uart_bridge/* "$INSTALL_DIR/uart_bridge/"
sudo chmod +x "$INSTALL_DIR/uart_bridge/"*.py

sudo cp t9_programmer/io_daemon/* "$INSTALL_DIR/io_daemon/"
sudo chmod +x "$INSTALL_DIR/io_daemon/"*.py

sudo cp t9_programmer/bin/* "$INSTALL_DIR/bin/"
sudo chmod +x "$INSTALL_DIR/bin/"*

# Link CLI tools to /usr/local/bin
sudo ln -sf "$INSTALL_DIR/bin/t9-io" /usr/local/bin/t9-io
sudo ln -sf "$INSTALL_DIR/bin/t9-reset" /usr/local/bin/t9-reset
sudo ln -sf "$INSTALL_DIR/scripts/t9_load_bit.py" /usr/local/bin/t9-load-bit

sudo chown -R xilinx:xilinx "$INSTALL_DIR"
sudo usermod -aG plugdev,dialout xilinx || true

# Install systemd services
echo "==> Installing systemd services..."
sudo cp t9_programmer/systemd/*.service /etc/systemd/system/
sudo systemctl daemon-reload

# Enable services
sudo systemctl enable t9-fpga.service
sudo systemctl enable t9-cmsis-dap.service
sudo systemctl enable t9-uart-bridge.service
sudo systemctl enable t9-io-daemon.service
sudo systemctl enable t9-programmer.service

# Start or restart services
echo "==> Starting services..."
sudo systemctl restart t9-fpga.service
sudo systemctl restart t9-cmsis-dap.service
sudo systemctl restart t9-uart-bridge.service
sudo systemctl restart t9-io-daemon.service

echo ""
echo "=========================================================================="
echo " DEPLOYMENT COMPLETED SUCCESSFULLY!"
echo "=========================================================================="
echo " - Universal CMSIS-DAP v2 (USB/IP): port 3240 (usbip attach -r <board_ip> -b 1-1)"
echo " - Web Dashboard & REST API:        http://$(hostname -I | awk '{print $1}'):8080"
echo " - Primary UART0 (TCP):             $(hostname -I | awk '{print $1}'):2000"
echo " - Secondary UART1 (TCP):           $(hostname -I | awk '{print $1}'):2001"
echo " - CLI Tools:                       t9-io list, t9-reset"
echo "=========================================================================="
