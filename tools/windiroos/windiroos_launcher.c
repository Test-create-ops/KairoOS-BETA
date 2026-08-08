/*
 * WindiroOS Windows launcher
 * A small Win32 GUI app that boots WindiroOS inside bundled QEMU.
 * Expects, next to windiroos.exe:
 *   - qemu\qemu-system-x86_64.exe
 *   - qemu\edk2-x86_64-code.fd   (OVMF, UEFI firmware)
 *   - viteza.iso                 (WindiroOS UEFI ISO)
 *
 * Cross-compile on macOS with mingw-w64:
 *   x86_64-w64-mingw32-gcc -O2 -mwindows -o windiroos.exe \
 *       windiroos_launcher.c -luser32 -lgdi32 -lcomctl32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <process.h>
#include <stdio.h>

#define APPNAME "WindiroOS"
#define DLG_BG   0x080818
#define DLG_FG   0xE8EEFF
#define ACCENT   0x3B82F6

#define BTN_START    1001
#define BTN_STOP     1002
#define CHK_FULL     1003
#define STAT_STATUS  1004
#define BTN_ABOUT    1005
#define RB_MEM_256   1006
#define RB_MEM_512   1007
#define RB_MEM_1024  1008
#define RB_MEM_2048  1009

static const int mem_choices[] = { 256, 512, 1024, 2048 };
#define MEM_DEFAULT  mem_choices[1]

static HWND g_hWnd, g_hBtnStart, g_hBtnStop, g_hChk, g_hStatus;
static HANDLE g_hProc = NULL;
static char g_exeDir[MAX_PATH];
static char g_status[512] = "Pronto. Premi Avvia per avviare WindiroOS.";

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    InvalidateRect(g_hStatus, NULL, TRUE);
}

static void find_base_dir(void)
{
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    char *slash = strrchr(buf, '\\');
    if (slash) *slash = 0;
    strcpy(g_exeDir, buf);
}

static void build_path(char *out, size_t n, const char *rel)
{
    _snprintf(out, n, "%s\\%s", g_exeDir, rel);
    out[n-1] = 0;
}

static int file_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI qemu_thread(void *arg)
{
    DWORD rc = WAIT_OBJECT_0;
    if (g_hProc) {
        WaitForSingleObject(g_hProc, INFINITE);
        GetExitCodeProcess(g_hProc, &rc);
        CloseHandle(g_hProc);
        g_hProc = NULL;
    }
    PostMessage(g_hWnd, WM_APP + 1, rc == 0 ? 0 : 1, 0);
    return 0;
}

static void start_windiroos(void)
{
    char qemu[MAX_PATH], ovmf[MAX_PATH], iso[MAX_PATH];
    build_path(qemu, sizeof qemu, "qemu\\qemu-system-x86_64.exe");
    build_path(ovmf, sizeof ovmf, "qemu\\edk2-x86_64-code.fd");
    build_path(iso,  sizeof iso,  "viteza.iso");

    if (!file_exists(qemu)) {
        set_status("QEMU non trovato accanto al programma.");
        return;
    }
    if (!file_exists(iso)) {
        set_status("viteza.iso non trovato accanto al programma.");
        return;
    }
    if (g_hProc) {
        set_status("WindiroOS è già in esecuzione.");
        return;
    }

    int mb = MEM_DEFAULT;
    if (IsDlgButtonChecked(g_hWnd, RB_MEM_256))  mb = mem_choices[0];
    else if (IsDlgButtonChecked(g_hWnd, RB_MEM_512))  mb = mem_choices[1];
    else if (IsDlgButtonChecked(g_hWnd, RB_MEM_1024)) mb = mem_choices[2];
    else if (IsDlgButtonChecked(g_hWnd, RB_MEM_2048)) mb = mem_choices[3];

    char cmd[2048];
    _snprintf(cmd, sizeof cmd,
        "\"%s\" "
        "-machine pc,accel=tcg -cpu qemu64 -m %d -vga std "
        "-audiodev dsound,id=audio0 -machine pcspk-audiodev=audio0 -device AC97,audiodev=audio0 "
        "-drive if=pflash,format=raw,readonly=on,file=\"%s\" "
        "-cdrom \"%s\" -boot d "
        "-display gtk,window-close=on",
        qemu, mb, ovmf, iso);

    if (SendMessageA(g_hChk, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        char *p = strchr(cmd, ' ');
        if (p) {
            char tmp[2048];
            strcpy(tmp, cmd);
            *p = 0;
            _snprintf(cmd, sizeof cmd, "\"%s\" -full-screen%s", tmp, p);
        }
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(qemu, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_exeDir, &si, &pi)) {
        char err[256];
        _snprintf(err, sizeof err, "Impossibile avviare QEMU (errore %lu).", GetLastError());
        set_status(err);
        return;
    }
    CloseHandle(pi.hThread);
    g_hProc = pi.hProcess;
    EnableWindow(g_hBtnStart, FALSE);
    EnableWindow(g_hBtnStop, TRUE);
    _beginthreadex(NULL, 0, (_beginthreadex_proc_type)qemu_thread, NULL, 0, NULL);
    set_status("WindiroOS in esecuzione... chiudi la finestra QEMU per tornare qui.");
}

static void stop_windiroos(void)
{
    if (g_hProc) {
        TerminateProcess(g_hProc, 0);
        WaitForSingleObject(g_hProc, INFINITE);
        CloseHandle(g_hProc);
        g_hProc = NULL;
    }
    EnableWindow(g_hBtnStart, TRUE);
    EnableWindow(g_hBtnStop, FALSE);
    set_status("WindiroOS fermato.");
}

static HBRUSH bg_brush(void)
{
    static HBRUSH br;
    if (!br) br = CreateSolidBrush(RGB((DLG_BG>>16)&255, (DLG_BG>>8)&255, DLG_BG&255));
    return br;
}

static void create_controls(void)
{
    HINSTANCE hi = GetModuleHandle(NULL);

    CreateWindowExA(0, "STATIC", "Memoria (MB):",
        WS_CHILD | WS_VISIBLE, 36, 168, 82, 20, g_hWnd, NULL, hi, NULL);
    CreateWindowExA(0, "BUTTON", "256",
        WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON, 122, 166, 46, 20, g_hWnd, (HMENU)RB_MEM_256, hi, NULL);
    CreateWindowExA(0, "BUTTON", "512",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 168, 166, 46, 20, g_hWnd, (HMENU)RB_MEM_512, hi, NULL);
    CreateWindowExA(0, "BUTTON", "1024",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 214, 166, 50, 20, g_hWnd, (HMENU)RB_MEM_1024, hi, NULL);
    CreateWindowExA(0, "BUTTON", "2048",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 264, 166, 52, 20, g_hWnd, (HMENU)RB_MEM_2048, hi, NULL);
    SendMessageA(GetDlgItem(g_hWnd, RB_MEM_512), BM_SETCHECK, BST_CHECKED, 0);
    {
        int ids[] = { RB_MEM_256, RB_MEM_512, RB_MEM_1024, RB_MEM_2048 };
        int i;
        for (i = 0; i < 4; i++)
            SendMessageA(GetDlgItem(g_hWnd, ids[i]), WM_SETFONT,
                (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }

    g_hChk = CreateWindowExA(0, "BUTTON", "A schermo intero",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 302, 220, 26, g_hWnd, (HMENU)CHK_FULL, hi, NULL);
    SendMessageA(g_hChk, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    g_hBtnStart = CreateWindowExA(0, "BUTTON", "Avvia WindiroOS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 36, 200, 240, 42, g_hWnd, (HMENU)BTN_START, hi, NULL);
    SendMessageA(g_hBtnStart, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    g_hBtnStop = CreateWindowExA(0, "BUTTON", "Stop",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 288, 200, 96, 42, g_hWnd, (HMENU)BTN_STOP, hi, NULL);
    EnableWindow(g_hBtnStop, FALSE);
    SendMessageA(g_hBtnStop, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    g_hStatus = CreateWindowExA(0, "STATIC", g_status,
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 36, 272, 340, 24, g_hWnd, (HMENU)STAT_STATUS, hi, NULL);
    SendMessageA(g_hStatus, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        create_controls();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case BTN_START: start_windiroos(); return 0;
        case BTN_STOP:  stop_windiroos();  return 0;
        case BTN_ABOUT:
            MessageBoxA(hwnd,
                "WindiroOS — sistema operativo hobby x86-64.\n"
                "Gira su QEMU (TCG) con firmware UEFI/OVMF.\n\n"
                "Single-developer, fatto con passione.",
                "Informazioni su WindiroOS", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;
    case WM_APP + 1:
        EnableWindow(g_hBtnStart, TRUE);
        EnableWindow(g_hBtnStop, FALSE);
        if (wp) set_status("QEMU terminato in modo anomalo (codice %lu).", (unsigned long)wp);
        else    set_status("WindiroOS chiuso normalmente.");
        return 0;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, RGB((DLG_BG>>16)&255, (DLG_BG>>8)&255, DLG_BG&255));
        SetTextColor(hdc, RGB((DLG_FG>>16)&255, (DLG_FG>>8)&255, DLG_FG&255));
        return (LRESULT)bg_brush();
    }
    case WM_ERASEBKGND:
        SetBkColor((HDC)wp, RGB((DLG_BG>>16)&255, (DLG_BG>>8)&255, DLG_BG&255));
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(hdc, &r, bg_brush());
        HPEN accent = CreatePen(PS_SOLID, 3, RGB((ACCENT>>16)&255, (ACCENT>>8)&255, ACCENT&255));
        HGDIOBJ old = SelectObject(hdc, accent);
        MoveToEx(hdc, 36, 150, NULL);
        LineTo(hdc, r.right - 36, 150);
        SelectObject(hdc, old);
        DeleteObject(accent);
        SetBkMode(hdc, TRANSPARENT);
        HFONT big = CreateFontA(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        HGDIOBJ oldf = SelectObject(hdc, big);
        SetTextColor(hdc, RGB((DLG_FG>>16)&255, (DLG_FG>>8)&255, DLG_FG&255));
        TextOutA(hdc, 36, 28, "WindiroOS", 8);
        SelectObject(hdc, oldf);
        DeleteObject(big);
        SetTextColor(hdc, RGB(0x8A, 0x9A, 0xCE));
        TextOutA(hdc, 36, 68, "Windows Launcher", 16);
        SetTextColor(hdc, RGB((DLG_FG>>16)&255, (DLG_FG>>8)&255, DLG_FG&255));
        TextOutA(hdc, 36, 118,
            "Avvia WindiroOS in una finestra virtuale (QEMU).", 47);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        stop_windiroos();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    find_base_dir();

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "WindiroOSLauncherWnd";
    RegisterClassA(&wc);

    RECT r = {0, 0, 400, 360};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

    g_hWnd = CreateWindowExA(0, "WindiroOSLauncherWnd", "WindiroOS",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        NULL, NULL, hi, NULL);
    if (!g_hWnd) return 1;

    ShowWindow(g_hWnd, show);
    UpdateWindow(g_hWnd);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
