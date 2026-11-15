package main

import (
	"bytes"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

const (
	BOARD_IP     = "192.168.2.99"
	DAP_PORT     = 3240
	WEB_PORT     = 8080
	USBIP_PATH   = `C:\Program Files\USBip\usbip.exe`
	APP_NAME     = "Plink"
	WINDOW_CLASS = "PLINK_TRAY_CLASS"
)

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	shell32  = syscall.NewLazyDLL("shell32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")

	pRegisterClassExW   = user32.NewProc("RegisterClassExW")
	pCreateWindowExW    = user32.NewProc("CreateWindowExW")
	pDefWindowProcW     = user32.NewProc("DefWindowProcW")
	pGetMessageW        = user32.NewProc("GetMessageW")
	pTranslateMessage   = user32.NewProc("TranslateMessage")
	pDispatchMessageW   = user32.NewProc("DispatchMessageW")
	pPostQuitMessage    = user32.NewProc("PostQuitMessage")
	pCreatePopupMenu    = user32.NewProc("CreatePopupMenu")
	pAppendMenuW        = user32.NewProc("AppendMenuW")
	pTrackPopupMenu     = user32.NewProc("TrackPopupMenu")
	pSetForegroundWindow= user32.NewProc("SetForegroundWindow")
	pDestroyMenu        = user32.NewProc("DestroyMenu")
	pGetCursorPos       = user32.NewProc("GetCursorPos")
	pLoadIconW          = user32.NewProc("LoadIconW")
	pLoadImageW         = user32.NewProc("LoadImageW")
	pShell_NotifyIconW  = shell32.NewProc("Shell_NotifyIconW")
	pGetModuleHandleW   = kernel32.NewProc("GetModuleHandleW")
)

const (
	WM_USER            = 0x0400
	WM_TRAYICON        = WM_USER + 100
	WM_COMMAND         = 0x0111
	WM_RBUTTONUP       = 0x0205
	WM_LBUTTONDBLCLK   = 0x0203

	NIM_ADD    = 0
	NIM_MODIFY = 1
	NIM_DELETE = 2

	NIF_MESSAGE = 0x0001
	NIF_ICON    = 0x0002
	NIF_TIP     = 0x0004
	NIF_INFO    = 0x0010

	NIIF_INFO   = 0x00000001
	NIIF_ERROR  = 0x00000003

	MF_STRING   = 0x00000000
	MF_SEPARATOR= 0x00000800
	MF_GRAYED   = 0x00000001
	MF_CHECKED  = 0x00000008

	TPM_RIGHTBUTTON = 0x0002

	IMAGE_ICON      = 1
	LR_LOADFROMFILE = 0x0010
	IDI_APPLICATION = 32512

	CMD_HEADER     = 1000
	CMD_ATTACH     = 1001
	CMD_DETACH     = 1002
	CMD_WEBUI      = 1003
	CMD_RESET      = 1004
	CMD_BOOTLOADER = 1005
	CMD_AUTOATTACH = 1006
	CMD_EXIT       = 1099
)

type POINT struct {
	X, Y int32
}

type WNDCLASSEXW struct {
	cbSize        uint32
	style         uint32
	lpfnWndProc   uintptr
	cbClsExtra    int32
	cbWndExtra    int32
	hInstance     uintptr
	hIcon         uintptr
	hCursor       uintptr
	hbrBackground uintptr
	lpszMenuName  *uint16
	lpszClassName *uint16
	hIconSm       uintptr
}

type NOTIFYICONDATAW struct {
	cbSize           uint32
	hWnd             uintptr
	uID              uint32
	uFlags           uint32
	uCallbackMessage uint32
	hIcon            uintptr
	szTip            [128]uint16
	dwState          uint32
	dwStateMask      uint32
	szInfo           [256]uint16
	uTimeoutOrVersion uint32
	szInfoTitle      [64]uint16
	dwInfoFlags      uint32
	guidItem         [16]byte
	hBalloonIcon     uintptr
}

type MSG struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	pt      POINT
}

var (
	hwndMain       uintptr
	nidMain        NOTIFYICONDATAW
	isAttached     bool
	isOnline       bool
	autoAttach     = true
	stateMu        sync.Mutex
	monitorCancel  chan struct{}
)

func utf16Ptr(s string) *uint16 {
	u := syscall.StringToUTF16(s)
	return &u[0]
}

func copyUTF16(dst []uint16, src string) {
	u := syscall.StringToUTF16(src)
	copy(dst, u)
}

func setTrayState(online bool, attached bool, balloonTitle string, balloonMsg string) {
	stateMu.Lock()
	defer stateMu.Unlock()

	isOnline = online
	isAttached = attached

	statusText := fmt.Sprintf("Plink Debug Pod [%s]\nStatus: ", BOARD_IP)
	if attached {
		statusText += "ATTACHED (CMSIS-DAP + COM Port)"
	} else if online {
		statusText += "ONLINE (Detached)"
	} else {
		statusText += "OFFLINE (Waiting for board)"
	}

	copyUTF16(nidMain.szTip[:], statusText)
	nidMain.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP

	if balloonTitle != "" && balloonMsg != "" {
		nidMain.uFlags |= NIF_INFO
		copyUTF16(nidMain.szInfoTitle[:], balloonTitle)
		copyUTF16(nidMain.szInfo[:], balloonMsg)
		if attached {
			nidMain.dwInfoFlags = NIIF_INFO
		} else {
			nidMain.dwInfoFlags = NIIF_ERROR
		}
	} else {
		nidMain.szInfo[0] = 0
		nidMain.szInfoTitle[0] = 0
		nidMain.dwInfoFlags = 0
	}

	pShell_NotifyIconW.Call(NIM_MODIFY, uintptr(unsafe.Pointer(&nidMain)))
}

func isUsbipPortAttached() bool {
	cmd := exec.Command(USBIP_PATH, "port")
	out, err := cmd.Output()
	if err != nil {
		return false
	}
	return bytes.Contains(out, []byte(BOARD_IP))
}

func attachUsbip() bool {
	if !isUsbipPortAttached() {
		cmd := exec.Command(USBIP_PATH, "attach", "--once", "-r", BOARD_IP, "-b", "1-1")
		cmd.Run()
	}
	return isUsbipPortAttached()
}

func detachUsbip() {
	cmd := exec.Command(USBIP_PATH, "detach", "-p", "1")
	cmd.Run()
}

func apiPost(endpoint string) {
	go func() {
		url := fmt.Sprintf("http://%s:%d%s", BOARD_IP, WEB_PORT, endpoint)
		req, err := http.NewRequest("POST", url, bytes.NewBuffer([]byte("{}")))
		if err != nil {
			return
		}
		req.Header.Set("Content-Type", "application/json")
		client := http.Client{Timeout: 2 * time.Second}
		client.Do(req)
	}()
}

// Event-driven monitor: Holds a single passive socket connection.
// Zero CPU polling: blocks on network socket events.
func startEventDrivenMonitor() {
	go func() {
		for {
			targetAddr := fmt.Sprintf("%s:%d", BOARD_IP, DAP_PORT)
			conn, err := net.DialTimeout("tcp", targetAddr, 1500*time.Millisecond)
			if err != nil {
				// Board is currently offline
				setTrayState(false, false, "", "")
				// Relaxed backoff: wait 4 seconds before re-checking
				time.Sleep(4 * time.Second)
				continue
			}

			// Connected to board!
			attached := false
			if autoAttach {
				attached = attachUsbip()
			} else {
				attached = isUsbipPortAttached()
			}

			if attached {
				setTrayState(true, true, "Plink Connected", "Plink Pod attached: CMSIS-DAP + Virtual COM Port ready.")
			} else {
				setTrayState(true, false, "Plink Online", "Board is online at 192.168.2.99")
			}

			// PASSIVE EVENT WAIT: Block on reading the socket.
			// ZERO polling packets are transmitted. When board powers off/reboots,
			// the OS TCP stack receives FIN/RST and Read() returns immediately.
			buf := make([]byte, 1)
			conn.SetReadDeadline(time.Time{}) // No timeout - purely event-driven
			_, rerr := conn.Read(buf)

			conn.Close()

			// Socket broke: Board disconnected or reset!
			if rerr == io.EOF || rerr != nil {
				setTrayState(false, false, "Plink Disconnected", "Connection to Plink pod lost.")
			}

			// Small pause to allow board network interface to reset cleanly
			time.Sleep(2 * time.Second)
		}
	}()
}

func showContextMenu(hwnd uintptr) {
	hMenu, _, _ := pCreatePopupMenu.Call()
	if hMenu == 0 {
		return
	}
	defer pDestroyMenu.Call(hMenu)

	stateMu.Lock()
	attached := isAttached
	online := isOnline
	auto := autoAttach
	stateMu.Unlock()

	statusHeader := "Plink: OFFLINE"
	if attached {
		statusHeader = fmt.Sprintf("Plink: CONNECTED [%s]", BOARD_IP)
	} else if online {
		statusHeader = fmt.Sprintf("Plink: ONLINE [%s]", BOARD_IP)
	}

	pAppendMenuW.Call(hMenu, MF_STRING|MF_GRAYED, CMD_HEADER, uintptr(unsafe.Pointer(utf16Ptr(statusHeader))))
	pAppendMenuW.Call(hMenu, MF_SEPARATOR, 0, 0)

	autoFlags := uintptr(MF_STRING)
	if auto {
		autoFlags |= MF_CHECKED
	}
	pAppendMenuW.Call(hMenu, autoFlags, CMD_AUTOATTACH, uintptr(unsafe.Pointer(utf16Ptr("Auto-Attach on Detect"))))

	if attached {
		pAppendMenuW.Call(hMenu, MF_STRING, CMD_DETACH, uintptr(unsafe.Pointer(utf16Ptr("Detach USB/IP Device"))))
	} else {
		pAppendMenuW.Call(hMenu, MF_STRING, CMD_ATTACH, uintptr(unsafe.Pointer(utf16Ptr("Attach USB/IP Device Now"))))
	}

	pAppendMenuW.Call(hMenu, MF_SEPARATOR, 0, 0)
	pAppendMenuW.Call(hMenu, MF_STRING, CMD_WEBUI, uintptr(unsafe.Pointer(utf16Ptr("Open Web Flasher & Dashboard (:8080)"))))
	pAppendMenuW.Call(hMenu, MF_STRING, CMD_RESET, uintptr(unsafe.Pointer(utf16Ptr("Target: Pulse nRST (50ms)"))))
	pAppendMenuW.Call(hMenu, MF_STRING, CMD_BOOTLOADER, uintptr(unsafe.Pointer(utf16Ptr("Target: Trigger Bootloader Mode"))))

	pAppendMenuW.Call(hMenu, MF_SEPARATOR, 0, 0)
	pAppendMenuW.Call(hMenu, MF_STRING, CMD_EXIT, uintptr(unsafe.Pointer(utf16Ptr("Exit Plink Tray"))))

	var pt POINT
	pGetCursorPos.Call(uintptr(unsafe.Pointer(&pt)))

	pSetForegroundWindow.Call(hwnd)
	pTrackPopupMenu.Call(hMenu, TPM_RIGHTBUTTON, uintptr(pt.X), uintptr(pt.Y), 0, hwnd, 0)
}

func wndProc(hwnd uintptr, msg uint32, wParam uintptr, lParam uintptr) uintptr {
	switch msg {
	case WM_TRAYICON:
		switch lParam {
		case WM_RBUTTONUP:
			showContextMenu(hwnd)
			return 0
		case WM_LBUTTONDBLCLK:
			exec.Command("rundll32", "url.dll,FileProtocolHandler", fmt.Sprintf("http://%s:%d", BOARD_IP, WEB_PORT)).Start()
			return 0
		}
	case WM_COMMAND:
		cmdId := int(wParam & 0xFFFF)
		switch cmdId {
		case CMD_ATTACH:
			go func() {
				if attachUsbip() {
					setTrayState(true, true, "Plink Attached", "Plink Pod attached successfully.")
				}
			}()
		case CMD_DETACH:
			go func() {
				detachUsbip()
				setTrayState(isOnline, false, "Plink Detached", "Plink Pod detached from Windows.")
			}()
		case CMD_AUTOATTACH:
			stateMu.Lock()
			autoAttach = !autoAttach
			stateMu.Unlock()
		case CMD_WEBUI:
			exec.Command("rundll32", "url.dll,FileProtocolHandler", fmt.Sprintf("http://%s:%d", BOARD_IP, WEB_PORT)).Start()
		case CMD_RESET:
			apiPost("/api/programmer/reset")
		case CMD_BOOTLOADER:
			apiPost("/api/programmer/bootloader")
		case CMD_EXIT:
			pShell_NotifyIconW.Call(NIM_DELETE, uintptr(unsafe.Pointer(&nidMain)))
			pPostQuitMessage.Call(0)
			return 0
		}
	}

	ret, _, _ := pDefWindowProcW.Call(hwnd, uintptr(msg), wParam, lParam)
	return ret
}

func main() {
	runtime.LockOSThread()

	hInstance, _, _ := pGetModuleHandleW.Call(0)

	var hIcon uintptr
	exePath, err := os.Executable()
	var icoPath string
	if err == nil {
		icoPath = filepath.Join(filepath.Dir(exePath), "plink.ico")
	} else {
		icoPath = "plink.ico"
	}
	hIcon, _, _ = pLoadImageW.Call(0, uintptr(unsafe.Pointer(utf16Ptr(icoPath))), uintptr(IMAGE_ICON), 32, 32, uintptr(LR_LOADFROMFILE))
	if hIcon == 0 {
		hIcon, _, _ = pLoadIconW.Call(0, uintptr(IDI_APPLICATION))
	}

	var wc WNDCLASSEXW
	wc.cbSize = uint32(unsafe.Sizeof(wc))
	wc.lpfnWndProc = syscall.NewCallback(wndProc)
	wc.hInstance = hInstance
	wc.lpszClassName = utf16Ptr(WINDOW_CLASS)
	wc.hIcon = hIcon

	pRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))

	hwnd, _, _ := pCreateWindowExW.Call(
		0,
		uintptr(unsafe.Pointer(utf16Ptr(WINDOW_CLASS))),
		uintptr(unsafe.Pointer(utf16Ptr(APP_NAME))),
		0,
		0, 0, 0, 0,
		0, 0, hInstance, 0,
	)

	if hwnd == 0 {
		fmt.Fprintf(os.Stderr, "Failed to create message window.\n")
		return
	}
	hwndMain = hwnd

	nidMain.cbSize = uint32(unsafe.Sizeof(nidMain))
	nidMain.hWnd = hwnd
	nidMain.uID = 1
	nidMain.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP
	nidMain.uCallbackMessage = WM_TRAYICON
	nidMain.hIcon = hIcon
	copyUTF16(nidMain.szTip[:], "Plink Debug Pod [Initializing...]")

	pShell_NotifyIconW.Call(NIM_ADD, uintptr(unsafe.Pointer(&nidMain)))

	// Start event-driven connection watcher
	startEventDrivenMonitor()

	// Win32 message loop
	var m MSG
	for {
		ret, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(ret) <= 0 {
			break
		}
		pTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		pDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
}
