package main

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
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
	pCreateMutexW      = kernel32.NewProc("CreateMutexW")
	pCloseHandle       = kernel32.NewProc("CloseHandle")
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
	connectionMu   sync.Mutex
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

	statusText := fmt.Sprintf("Plink Pod [%s]\nStatus: ", BOARD_IP)
	if attached {
		statusText += "Connected"
	} else {
		statusText += "Closed"
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

func runCmdSilent(name string, args ...string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.SysProcAttr = &syscall.SysProcAttr{
		HideWindow:    true,
		CreationFlags: 0x08000000, // CREATE_NO_WINDOW
	}
	return cmd.CombinedOutput()
}

func matchingPorts(out []byte) []int {
	var ports []int
	port := -1
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "Port ") {
			fields := strings.Fields(line)
			port, _ = strconv.Atoi(strings.TrimSuffix(fields[1], ":"))
		}
		if port >= 0 && strings.TrimSpace(strings.TrimPrefix(line, "->")) == "usbip://"+BOARD_IP+":3240/1-1" {
			ports = append(ports, port)
		}
	}
	return ports
}

func isUsbipPortAttached() bool {
	out, err := runCmdSilent(USBIP_PATH, "port")
	if err != nil {
		return false
	}
	return len(matchingPorts(out)) == 1
}

func detachUsbip() {
	connectionMu.Lock()
	defer connectionMu.Unlock()
	out, err := runCmdSilent(USBIP_PATH, "port")
	if err != nil { return }
	for _, port := range matchingPorts(out) {
		if _, err := runCmdSilent(USBIP_PATH, "detach", "-p", strconv.Itoa(port)); err != nil {
			return // No retries, hub resets, or unrelated process termination.
		}
	}
}

func attachUsbip() bool {
	connectionMu.Lock()
	defer connectionMu.Unlock()
	out, err := runCmdSilent(USBIP_PATH, "port")
	if err != nil { return false }
	ports := matchingPorts(out)
	if len(ports) != 0 { return len(ports) == 1 }

	// Attach Pod #1 (CMSIS-DAP + Dual COM) silently in background
	if _, err := runCmdSilent(USBIP_PATH, "attach", "--once", "-r", BOARD_IP, "-b", "1-1"); err != nil {
		return false
	}
	return isUsbipPortAttached()
}

func apiPost(endpoint string) {
	go func() {
		url := fmt.Sprintf("http://%s:%d%s", BOARD_IP, WEB_PORT, endpoint)
		req, err := http.NewRequest("POST", url, bytes.NewBuffer([]byte("{}")))
		if err != nil {
			return
		}
		req.Header.Set("Content-Type", "application/json")
		token := os.Getenv("PLINK_API_TOKEN")
		if token == "" {
			setTrayState(true, isUsbipPortAttached(), "Plink Control", "Set PLINK_API_TOKEN to enable target control.")
			return
		}
		req.Header.Set("Authorization", "Bearer "+token)
		client := http.Client{Timeout: 2 * time.Second}
		resp, err := client.Do(req)
		if err == nil { resp.Body.Close() }
	}()
}

// Event-driven monitor: Uses HTTP SSE stream to monitor board online state.
// Does NOT touch port 3240, keeping USB/IP port dedicated.
func startEventDrivenMonitor() {
	go func() {
		client := &http.Client{Transport: &http.Transport{
			DialContext: (&net.Dialer{Timeout: 3*time.Second, KeepAlive: 30*time.Second}).DialContext,
			ResponseHeaderTimeout: 3*time.Second,
		}}
		for {
			url := fmt.Sprintf("http://%s:%d/api/log_stream", BOARD_IP, WEB_PORT)
			resp, err := client.Get(url)
			if err != nil {
				// Board is currently offline
				setTrayState(false, false, "", "")
				time.Sleep(3 * time.Second)
				continue
			}
			if resp.StatusCode != http.StatusOK {
				resp.Body.Close()
				time.Sleep(3 * time.Second)
				continue
			}

			// Connected to board!
			attached := false
			stateMu.Lock()
			auto := autoAttach
			stateMu.Unlock()
			if auto {
				attached = attachUsbip()
			} else {
				attached = isUsbipPortAttached()
			}

			if attached {
				setTrayState(true, true, "Plink Connected", "Pod connected (CMSIS-DAP + Dual COM).")
			} else {
				setTrayState(true, false, "Plink Connected", "Pod online at 192.168.2.99")
			}

			// Block passively on reading the SSE stream. Zero CPU and zero polling.
			buf := make([]byte, 512)
			for {
				_, rerr := resp.Body.Read(buf)
				if rerr != nil {
					break
				}
			}
			resp.Body.Close()

			setTrayState(false, false, "Plink Closed", "Connection closed.")
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

	statusHeader := "Plink: Closed"
	if attached {
		statusHeader = fmt.Sprintf("Plink: Connected [%s]", BOARD_IP)
	} else if online {
		statusHeader = fmt.Sprintf("Plink: Connected (Detached) [%s]", BOARD_IP)
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
					setTrayState(true, true, "Plink Connected", "Pod attached successfully.")
				}
			}()
		case CMD_DETACH:
			go func() {
				stateMu.Lock()
				autoAttach = false
				online := isOnline
				stateMu.Unlock()
				detachUsbip()
				setTrayState(online, false, "Plink Closed", "Devices detached.")
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
	mutex, _, errMutex := pCreateMutexW.Call(0, 0, uintptr(unsafe.Pointer(utf16Ptr("Local\\PlinkTraySingleton"))))
	if mutex == 0 || errMutex == syscall.Errno(183) { return }
	defer pCloseHandle.Call(mutex)

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
