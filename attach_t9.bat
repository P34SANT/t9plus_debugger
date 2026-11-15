@echo off
title Connect Plink Debugger Pod
echo ========================================================
echo  Plink USB/IP Connection Utility (CMSIS-DAP + COM Port)
echo ========================================================

echo [*] Checking current USB/IP connection...
"C:\Program Files\USBip\usbip.exe" port | findstr /c:"192.168.2.99" >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [+] Plink probe is ALREADY CONNECTED!
    echo     Ready for Keil uVision, IAR EWARM, OpenOCD, and pyOCD.
    goto :done
)

echo [*] Pinging Plink pod at 192.168.2.99...
ping -n 1 -w 1000 192.168.2.99 >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [-] Error: Plink pod at 192.168.2.99 is not responding.
    echo     Please check board power and Ethernet cable.
    pause
    exit /b 1
)

echo [*] Attaching Plink composite device (busid 1-1)...
"C:\Program Files\USBip\usbip.exe" attach --once -r 192.168.2.99 -b 1-1

"C:\Program Files\USBip\usbip.exe" port | findstr /c:"192.168.2.99" >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [+] SUCCESS: Plink pod attached to Windows!
    echo     CMSIS-DAP v2 (WinUSB) + Native COM Port Active.
    echo     Ready for Keil uVision, IAR EWARM, OpenOCD, and pyOCD.
) else (
    echo [-] Failed to attach device. Please verify board service status.
)

:done
ping 127.0.0.1 -n 3 >nul 2>&1
