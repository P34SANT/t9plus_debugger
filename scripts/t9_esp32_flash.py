#!/usr/bin/env python3
"""
t9_esp32_flash.py - Automated Network ESP32 / ESP8266 Programmer for Antminer T9+
Communicates over the Antminer T9+ TCP UART Bridge & I/O Daemon REST API.

Pin Wiring (J3 Header):
  - J3 Pin 7  (TX / C20)   -> ESP32 RXD0
  - J3 Pin 8  (RX / B20)   -> ESP32 TXD0
  - J3 Pin 5  (nRST / T14) -> ESP32 EN (CHIP_PU)
  - J3 Pin 15 (IO0 / T15)  -> ESP32 GPIO0
  - J3 Pin 1 / 2           -> GND
  - J3 Pin 23 / 24         -> 3.3V (optional target power)

Usage Examples:
  # Read Chip Flash Information
  python t9_esp32_flash.py --ip 192.168.2.99 flash_id

  # Flash Binary Firmware
  python t9_esp32_flash.py --ip 192.168.2.99 write_flash 0x10000 app.bin

  # Erase Chip
  python t9_esp32_flash.py --ip 192.168.2.99 erase_flash
"""
import sys
import os
import argparse
import subprocess
import urllib.request
import json
import time

def enter_bootloader(ip, port=8080):
    url = f"http://{ip}:{port}/api/esp32/bootloader"
    req = urllib.request.Request(url, data=b"{}", headers={'Content-Type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            data = json.loads(resp.read().decode())
            return data.get("status") == "ok"
    except Exception as e:
        print(f"[-] Warning: Failed to trigger hardware bootloader sequence: {e}", file=sys.stderr)
        return False

def reset_target(ip, port=8080):
    url = f"http://{ip}:{port}/api/esp32/reset"
    req = urllib.request.Request(url, data=b"{}", headers={'Content-Type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            data = json.loads(resp.read().decode())
            return data.get("status") == "ok"
    except Exception as e:
        print(f"[-] Warning: Failed to trigger target reset: {e}", file=sys.stderr)
        return False

def main():
    parser = argparse.ArgumentParser(description="Automated Antminer T9+ ESP32 Programmer", add_help=False)
    parser.add_argument("--ip", default=os.environ.get("T9_IP", "192.168.2.99"), help="Antminer T9+ IP Address")
    parser.add_argument("--tcp-port", type=int, default=2000, help="TCP UART Port (default: 2000)")
    parser.add_argument("--api-port", type=int, default=8080, help="I/O REST API Port (default: 8080)")
    parser.add_argument("--no-reset", action="store_true", help="Do not pulse EN reset after flashing")
    
    args, esptool_args = parser.parse_known_args()

    if not esptool_args or "-h" in esptool_args or "--help" in esptool_args:
        parser.print_help()
        print("\nAll additional arguments are forwarded directly to esptool.py.")
        sys.exit(0)

    print(f"[*] Antminer T9+ Network ESP32 Programmer")
    print(f"[*] Board IP: {args.ip} | UART Port: {args.tcp_port} | REST Port: {args.api_port}")
    
    print("[*] Placing ESP32 into ROM Download Bootloader mode...")
    if enter_bootloader(args.ip, args.api_port):
        print("[+] ESP32 bootloader hardware handshake SUCCESS!")
    else:
        print("[!] Manual bootloader entry may be required if REST API is unreachable.")

    time.sleep(0.1)

    cmd = [
        sys.executable, "-m", "esptool",
        "--port", f"socket://{args.ip}:{args.tcp_port}",
        "--baud", "115200",
        "--before", "no_reset",
        "--after", "no_reset"
    ] + esptool_args

    print(f"[*] Running esptool: {' '.join(cmd)}")
    ret = subprocess.run(cmd)

    if not args.no_reset:
        print("[*] Resetting ESP32 into normal execution mode...")
        reset_target(args.ip, args.api_port)
        print("[+] Target reset complete. ESP32 is now running.")

    sys.exit(ret.returncode)

if __name__ == "__main__":
    main()
