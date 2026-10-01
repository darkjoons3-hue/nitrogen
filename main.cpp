#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <mmsystem.h>
#include <magnification.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")

// ================= КОНФИГ =================
namespace Cfg {
    constexpr int kPhaseGrayMs     = 16000;
    constexpr int kPhaseRainbowMs  = 24000;
    constexpr int kPhaseSlideMs    = 20000;
    constexpr int kPhaseGlitchMs   = 30000;
    constexpr int kPhaseGray2Ms    = 3000;
    constexpr int kPhaseErrorsMs   = 10000;
    constexpr int kPhaseFinalMs    = 6000;
    constexpr int kCmdCount        = 12;
    constexpr int kErrCount        = 32;
    constexpr int kMaxPrankMs      = 300000;  // аварийный watchdog: 5 минут
}

// ================= ГЛОБАЛЫ =================
std::atomic<bool> g_stop{false};
std::atomic<bool> g_killSwitch{false};

HHOOK      g_kbHook  = nullptr;
HWND       g_magWnd  = nullptr;
HINSTANCE  g_hInst   = nullptr;

// Классы окон
const wchar_t* ERR_CLASS  = L"NitroErr";
const wchar_t* GLITCH_CLASS = L"NitroGlitch";
const wchar_t* SLIDE_CLASS  = L"NitroSlide";

// 3D-куб
struct CubeState {
    double angX = 0.0, angY = 0.0;
    double x = 0.0, y = 0.0;
    double size = 70.0;
};
CubeState g_cube;
POINT     g_cursor = {0, 0};

// ================= УТИЛИТЫ =================
void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
void SleepPump(int ms) {
    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < ms) {
        PumpMessages();
        Sleep(15);
    }
}

// ================= ПРОВЕРКА ПРАВ =================
bool IsElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    TOKEN_ELEVATION el = {};
    DWORD sz = sizeof(el);
    bool ok = GetTokenInformation(hToken, TokenElevation, &el, sz, &sz)
              && el.TokenIsElevated;
    CloseHandle(hToken);
    return ok;
}

void RelaunchElevated() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow  = SW_SHOW;
    ShellExecuteExW(&sei);
}

// ================= KEYBOARD HOOK =================
LRESULT CALLBACK LLKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
            wParam == WM_KEYUP   || wParam == WM_SYSKEYUP) {
            // Ctrl+Alt+Del сюда не доходит — обрабатывает Winlogon
            return 1;
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

void InstallKbHook() {
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboardProc, nullptr, 0);
}
void RemoveKbHook() {
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
}

// ================= MAGNIFIER / ЦВЕТОВОЙ ФИЛЬТР =================
MAGCOLOREFFECT g_grayEffect = {
    0.30f, 0.30f, 0.30f, 0.0f, 0.0f,
    0.59f, 0.59f, 0.59f, 0.0f, 0.0f,
    0.11f, 0.11f, 0.11f, 0.0f, 0.0f,
    0.0f,  0.0f,  0.0f,  1.0f, 0.0f,
    0.0f,  0.0f,  0.0f,  0.0f, 1.0f
};
MAGCOLOREFFECT g_rainbowEffect = {
    1.2f, 0.4f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.2f, 0.4f, 0.0f, 0.0f,
    0.4f, 0.0f, 1.2f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 1.0f
};
MAGCOLOREFFECT g_identityEffect = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 1.0f
};

void InitMagnifier() {
    MagInitialize();
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_magWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        WC_MAGNIFIER, L"", WS_POPUP,
        0, 0, sw, sh, nullptr, nullptr, g_hInst, nullptr);
    if (g_magWnd) {
        MAGTRANSFORM t = { 1,0,0, 0,1,0, 0,0,1 };
        MagSetWindowTransform(g_magWnd, &t);
        RECT src = { 0, 0, sw, sh };
        MagSetWindowSource(g_magWnd, src);
    }
}
void SetColorEffect(const MAGCOLOREFFECT* e) {
    if (g_magWnd) MagSetColorEffect(g_magWnd, e);
}
void ShowMagnifier(bool on) {
    if (g_magWnd) ShowWindow(g_magWnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}
void ShutdownMagnifier() {
    if (g_magWnd) { DestroyWindow(g_magWnd); g_magWnd = nullptr; }
    MagUninitialize();
}

// ================= ТРЯСКА ЭКРАНА =================
void ShakeScreen(int durationMs, int intensity) {
    HDC hdc = GetDC(nullptr);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        int dx = (rand() % (intensity * 2 + 1)) - intensity;
        int dy = (rand() % (intensity * 2 + 1)) - intensity;
        BitBlt(hdc, dx, dy, sw, sh, hdc, 0, 0, SRCCOPY);
        Sleep(20);
    }
    ReleaseDC(nullptr, hdc);
}

// ================= БАС / МУЗЫКА =================
void BassThread(int durationMs) {
    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        Beep(40, 250);
        Beep(55, 200);
        Beep(35, 300);
    }
}

void PlayFurElise() {
    int seq[][2] = {
        {659,150},{622,150},{659,150},{622,150},{659,150},
        {494,150},{587,150},{523,150},{440,300},
        {262,150},{330,150},{440,150},
        {494,300},
        {330,150},{415,150},{494,150},
        {523,300},
        {330,150},{659,150},{622,150},{659,150},{622,150},
        {659,150},{494,150},{587,150},{523,150},{440,500}
    };
    for (auto& n : seq) {
        if (g_stop.load()) break;
        Beep(n[0], n[1]);
    }
}
void PianoThread(int durationMs) {
    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        PlayFurElise();
    }
}

void Play8Bit() {
    // Простая «чиптюн» мелодия
    int seq[][2] = {
        {523,100},{659,100},{784,100},{1047,200},
        {988,100},{784,100},{659,200},
        {523,100},{659,100},{784,100},{880,200},
        {784,100},{659,100},{523,300}
    };
    for (auto& n : seq) {
        if (g_stop.load()) break;
        Beep(n[0], n[1]);
    }
}
void EightBitThread(int durationMs) {
    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        Play8Bit();
    }
}

// ================= 3D КУБ =================
struct V3 { double x, y, z; };

void DrawCube(HDC hdc, int cx, int cy, double size, double ax, double ay) {
    V3 v[8] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
    };
    for (auto& p : v) {
        p.x *= size; p.y *= size; p.z *= size;
        double tx =  p.x*cos(ay) - p.z*sin(ay);
        double tz =  p.x*sin(ay) + p.z*cos(ay);
        p.x = tx; p.z = tz;
        double ty =  p.y*cos(ax) - p.z*sin(ax);
        p.z =  p.y*sin(ax) + p.z*cos(ax);
        p.y = ty;
    }
    POINT pt[8];
    for (int i = 0; i < 8; ++i) {
        pt[i].x = cx + (int)v[i].x;
        pt[i].y = cy + (int)v[i].y;
    }
    int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
    HPEN op  = (HPEN)SelectObject(hdc, pen);
    for (int i = 0; i < 12; ++i) {
        MoveToEx(hdc, pt[edges[i][0]].x, pt[edges[i][0]].y, nullptr);
        LineTo  (hdc, pt[edges[i][1]].x, pt[edges[i][1]].y);
    }
    SelectObject(hdc, op);
    DeleteObject(pen);
}

void CubeOverlayThread(int durationMs) {
    HDC hdc = GetDC(nullptr);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    // Хвост (радужный след)
    std::vector<POINT> trail;
    trail.reserve(120);

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        // Считываем позицию курсора
        POINT cur;
        GetCursorPos(&cur);
        g_cursor = cur;

        // Плавное движение куба к курсору
        g_cube.x += (cur.x - g_cube.x) * 0.15;
        g_cube.y += (cur.y - g_cube.y) * 0.15;
        g_cube.angX += 0.05;
        g_cube.angY += 0.07;

        // Очищаем область вокруг (перерисовываем фон — просто перекрываем)
        // Просто рисуем поверх — фон остаётся, но куб движется
        // Для очистки предыдущего кадра берём скриншот и восстанавливаем
        // Упрощённо: рисуем всё заново с "backing store"

        // Радужный след
        trail.push_back({ (LONG)g_cube.x, (LONG)g_cube.y });
        if (trail.size() > 60) trail.erase(trail.begin());

        for (size_t i = 0; i < trail.size(); ++i) {
            double t = (double)i / (double)trail.size();
            int r = (int)(255 * std::abs(sin(t * 6.28 + 0.0)));
            int g = (int)(255 * std::abs(sin(t * 6.28 + 2.1)));
            int b = (int)(255 * std::abs(sin(t * 6.28 + 4.2)));
            HBRUSH br = CreateSolidBrush(RGB(r, g, b));
            HPEN   pn = CreatePen(PS_SOLID, 1, RGB(r, g, b));
            HGDIOBJ ob = SelectObject(hdc, br);
            HGDIOBJ op = SelectObject(hdc, pn);
            int sz = (int)(3 + t * 6);
            Ellipse(hdc, trail[i].x - sz, trail[i].y - sz,
                          trail[i].x + sz, trail[i].y + sz);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(br);
            DeleteObject(pn);
        }

        DrawCube(hdc, (int)g_cube.x, (int)g_cube.y,
                 g_cube.size, g_cube.angX, g_cube.angY);

        Sleep(15);
    }
    ReleaseDC(nullptr, hdc);
}

// ================= CMD-ОКНА =================
void SpawnCmdWindow(const wchar_t* text) {
    wchar_t cmdLine[512];
    swprintf_s(cmdLine, L"cmd.exe /k echo %s", text);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
                   CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);
}

void CmdSpamThread(int count, int durationMs) {
    int perCmd = durationMs / (count > 0 ? count : 1);
    for (int i = 0; i < count && !g_stop.load(); ++i) {
        SpawnCmdWindow(L"YOUR IDIOT))");
        SleepPump(perCmd);
    }
}

// ================= ОКНА ОШИБОК =================
LRESULT CALLBACK ErrProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        // Заголовок
        HBRUSH tb = CreateSolidBrush(RGB(0, 90, 158));
        RECT tr = { 0, 0, rc.right, 28 };
        FillRect(dc, &tr, tb);
        DeleteObject(tb);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255,255,255));
        HFONT ft = CreateFontW(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
        HFONT of = (HFONT)SelectObject(dc, ft);
        RECT trt = { 8, 5, rc.right - 30, 25 };
        DrawTextW(dc, L"nitrogenError.exe", -1, &trt, DT_LEFT | DT_SINGLELINE);
        // Основной текст
        SetTextColor(dc, RGB(0,0,0));
        HFONT fb = CreateFontW(22,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
        SelectObject(dc, fb);
        RECT br = { 10, 40, rc.right - 10, rc.bottom - 10 };
        DrawTextW(dc, L"эщкере))", -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        DeleteObject(ft);
        DeleteObject(fb);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

void RegisterErrClass() {
    WNDCLASSW wc = {};
    wc.hInstance = g_hInst;
    wc.lpfnWndProc = ErrProc;
    wc.lpszClassName = ERR_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
}

void ShowErrorWindow(int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        ERR_CLASS, L"nitrogenError.exe",
        WS_POPUP, x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
}

// 32 ошибки последовательно, заполняя экран
void ErrorsFillDesktop(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = sw / 8;
    int wh = sh / 4;
    int cols = sw / ww;
    int rows = sh / wh;
    int total = cols * rows;
    if (total > Cfg::kErrCount) total = Cfg::kErrCount;

    int perMs = durationMs / (total > 0 ? total : 1);
    for (int i = 0; i < total && !g_stop.load(); ++i) {
        int cx = (i % cols) * ww;
        int cy = (i / cols) * wh;
        ShowErrorWindow(cx, cy, ww, wh);
        SleepPump(perMs);
    }
}

// 3 «шумных» ошибки — с мерцанием (просто рандомная позиция, частое пересоздание)
void NoiseErrorsThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    DWORD start = GetTickCount();
    std::vector<HWND> errs;
    for (int i = 0; i < 3; ++i) {
        HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            ERR_CLASS, L"nitrogenError.exe",
            WS_POPUP, rand() % (sw - 300), rand() % (sh - 200), 300, 180,
            nullptr, nullptr, g_hInst, nullptr);
        ShowWindow(hw, SW_SHOWNOACTIVATE);
        errs.push_back(hw);
    }
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        for (HWND hw : errs) {
            int x = rand() % (sw - 300);
            int y = rand() % (sh - 200);
            SetWindowPos(hw, HWND_TOPMOST, x, y, 300, 180, SWP_NOACTIVATE);
        }
        Sleep(60);
    }
    for (HWND hw : errs) DestroyWindow(hw);
}

// ================= GDI-ГЛИТЧИ =================
void GlitchThread(int durationMs) {
    HDC hdc = GetDC(nullptr);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    DWORD start = GetTickCount();
    int tick = 0;
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        // Резкие инверсии
        for (int i = 0; i < 25; ++i) {
            int x = rand() % sw;
            int y = rand() % sh;
            int w = 60 + rand() % 500;
            int h = 40 + rand() % 250;
            RECT r = { x, y, x + w, y + h };
            InvertRect(hdc, &r);
        }
        // «Плывущие» копии
        for (int i = 0; i < 15; ++i) {
            int x = rand() % sw;
            int y = rand() % sh;
            int w = 100 + rand() % 400;
            int h = 60 + rand() % 300;
            int dx = (rand() % 9) - 4;
            int dy = (rand() % 9) - 4;
            BitBlt(hdc, x + dx, y + dy, w, h, hdc, x, y, SRCCOPY);
        }
        if (++tick % 4 == 0) {
            BitBlt(hdc, 0, 0, sw, sh, hdc, 0, 0, NOTSRCERASE);
        }
        Sleep(35);
    }
    ReleaseDC(nullptr, hdc);
}

// ================= SLIDE (лента рабочего стола) =================
HBITMAP CaptureScreen() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hSrc = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hSrc);
    HBITMAP bmp = CreateCompatibleBitmap(hSrc, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);
    BitBlt(hMem, 0, 0, sw, sh, hSrc, 0, 0, SRCCOPY);
    SelectObject(hMem, old);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hSrc);
    return bmp;
}

// Класс окна, которое показывает снимок и умеет «уезжать»
struct SlideData {
    HBITMAP bmp = nullptr;
    int offsetX = 0;
    int bmpW = 0, bmpH = 0;
};
SlideData g_slideA, g_slideB;

void DrawSlide(HDC dc, SlideData& sd, int x) {
    if (!sd.bmp) return;
    HDC hMem = CreateCompatibleDC(dc);
    HBITMAP old = (HBITMAP)SelectObject(hMem, sd.bmp);
    BitBlt(dc, x, 0, sd.bmpW, sd.bmpH, hMem, 0, 0, SRCCOPY);
    SelectObject(hMem, old);
    DeleteDC(hMem);
}

LRESULT CALLBACK SlideProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        // Задний фон — второй снимок (он как бы «за» первым)
        DrawSlide(dc, g_slideB, 0);
        // Передний снимок — сдвинут влево
        DrawSlide(dc, g_slideA, -g_slideA.offsetX);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

void RegisterSlideClass() {
    WNDCLASSW wc = {};
    wc.hInstance = g_hInst;
    wc.lpfnWndProc = SlideProc;
    wc.lpszClassName = SLIDE_CLASS;
    RegisterClassW(&wc);
}

void SlideDesktopThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_slideA.bmp = CaptureScreen();
    g_slideA.bmpW = sw; g_slideA.bmpH = sh;
    g_slideA.offsetX = 0;

    g_slideB.bmp = CaptureScreen();
    g_slideB.bmpW = sw; g_slideB.bmpH = sh;

    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        SLIDE_CLASS, L"", WS_POPUP, 0, 0, sw, sh,
        nullptr, nullptr, g_hInst, nullptr);
    ShowWindow(hw, SW_SHOWNOACTIVATE);

    DWORD start = GetTickCount();
    double speed = 0.5;
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        g_slideA.offsetX += (int)speed;
        if (g_slideA.offsetX > sw) g_slideA.offsetX = 0;
        speed *= 1.02;  // ускорение
        if (speed > 25.0) speed = 25.0;
        InvalidateRect(hw, nullptr, FALSE);
        UpdateWindow(hw);
        PumpMessages();
        Sleep(20);
    }
    DestroyWindow(hw);
    if (g_slideA.bmp) { DeleteObject(g_slideA.bmp); g_slideA.bmp = nullptr; }
    if (g_slideB.bmp) { DeleteObject(g_slideB.bmp); g_slideB.bmp = nullptr; }
}

// ================= ФИНАЛ =================
void KillExplorer() {
    // Аккуратно через CreateProcess (без system())
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"cmd.exe /c taskkill /f /im explorer.exe >nul 2>&1";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void ShowFinalText() {
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(path, L"%snitrogen_final.txt", tmp);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const char* msg = "\xEF\xBB\xBF"  // UTF-8 BOM
            "Спасибо что вы были с нами";
        DWORD wr;
        WriteFile(h, msg, (DWORD)strlen(msg), &wr, nullptr);
        CloseHandle(h);
    }
    ShellExecuteW(nullptr, L"open", L"notepad.exe", path, nullptr, SW_SHOW);
}

void DoReboot() {
    HANDLE hTok;
    TOKEN_PRIVILEGES tkp;
    if (OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) {
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
        tkp.PrivilegeCount = 1;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hTok, FALSE, &tkp, 0, nullptr, nullptr);
        CloseHandle(hTok);
    }
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, SHTDN_REASON_MAJOR_APPLICATION);
}

// ================= WATCHDOG =================
void WatchdogThread() {
    DWORD start = GetTickCount();
    while (true) {
        if (g_stop.load()) return;
        if ((int)(GetTickCount() - start) > Cfg::kMaxPrankMs) {
            // Аварийный выход: снимаем всё и перезагружаемся
            RemoveKbHook();
            BlockInput(FALSE);
            DoReboot();
            return;
        }
        Sleep(1000);
    }
}

// ================= ТОЧКА ВХОДА =================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;

    // Мьютекс — не даём запуститься дважды
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"NitrogenPrankMutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Если не админ — перезапускаемся с UAC-запросом (один раз)
    if (!IsElevated()) {
        RelaunchElevated();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    srand((unsigned)time(nullptr));

    // ===== Шаг 1: Предупреждение =====
    int r = MessageBoxW(nullptr,
        L"ВНИМАНИЕ!\n\n"
        L"Эта программа — БЕЗОБИДНЫЙ ПРАНК.\n"
        L"Она НЕ удалит ваши файлы, НЕ украдёт данные,\n"
        L"НЕ навредит вашему компьютеру/ноутбуку.\n\n"
        L"НО! Клавиатура будет временно заблокирована,\n"
        L"экран заполнится эффектами, а через ~2 минуты\n"
        L"произойдёт реальная перезагрузка.\n\n"
        L"СОХРАНИТЕ ВСЁ ОТКРЫТОЕ ПЕРЕД ЗАПУСКОМ!\n\n"
        L"Запустить?",
        L"nitrogen.exe — предупреждение",
        MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
    if (r != IDYES) {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    // ===== Шаг 2: Блокировки + начало эффекта =====
    InstallKbHook();
    BlockInput(TRUE);  // работает, потому что мы под админом

    RegisterErrClass();
    RegisterSlideClass();
    InitMagnifier();

    std::thread tWatch(WatchdogThread);

    // ----- ФАЗА 1: Grayscale + тряска + бас — 16 сек -----
    SetColorEffect(&g_grayEffect);
    ShowMagnifier(true);

    std::thread tBass1(BassThread, Cfg::kPhaseGrayMs);

    DWORD f1 = GetTickCount();
    while ((int)(GetTickCount() - f1) < Cfg::kPhaseGrayMs && !g_stop.load()) {
        // тряска
        HDC hdc = GetDC(nullptr);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        int dx = (rand() % 5) - 2;
        int dy = (rand() % 5) - 2;
        BitBlt(hdc, dx, dy, sw, sh, hdc, 0, 0, SRCCOPY);
        ReleaseDC(nullptr, hdc);
        Sleep(25);
    }
    tBass1.join();

    // ----- ФАЗА 2: Радуга + cmd + куб + пианино — 24 сек -----
    SetColorEffect(&g_rainbowEffect);
    std::thread tCmd(CmdSpamThread, Cfg::kCmdCount, Cfg::kPhaseRainbowMs);
    std::thread tCube(CubeOverlayThread, Cfg::kPhaseRainbowMs);
    std::thread tPiano(PianoThread, Cfg::kPhaseRainbowMs);
    SleepPump(Cfg::kPhaseRainbowMs);
    tCmd.join(); tCube.join(); tPiano.join();

    // ----- ФАЗА 3: Слайд рабочего стола + 3 шумные ошибки + звук с повышением -----
    ShowMagnifier(false);
    std::thread tNoise(NoiseErrorsThread, Cfg::kPhaseSlideMs);
    std::thread tSlide(SlideDesktopThread, Cfg::kPhaseSlideMs);
    // «Повышающийся» звук
    std::thread tRise([](int dur){
        DWORD s = GetTickCount();
        int freq = 200;
        while (!g_stop.load() && (int)(GetTickCount() - s) < dur) {
            Beep(freq, 200);
            freq += 40;
            if (freq > 2000) freq = 2000;
        }
    }, Cfg::kPhaseSlideMs);
    tNoise.join(); tSlide.join(); tRise.join();

    // ----- ФАЗА 4: Инверсия + глитчи + эщкере-ошибки + 8-битная музыка — 30 сек -----
    SetColorEffect(&g_identityEffect);
    std::thread tGlitch(GlitchThread, Cfg::kPhaseGlitchMs);
    std::thread tErr([](int dur){
        DWORD s = GetTickCount();
        while (!g_stop.load() && (int)(GetTickCount() - s) < dur) {
            int x = rand() % 800 - 200;
            int y = rand() % 500 - 100;
            ShowErrorWindow(x, y, 380, 220);
            Sleep(700);
        }
    }, Cfg::kPhaseGlitchMs);
    std::thread t8bit(EightBitThread, Cfg::kPhaseGlitchMs);
    tGlitch.join(); tErr.join(); t8bit.join();

    // ----- ФАЗА 5: Снова grayscale + тряска — 3 сек -----
    SetColorEffect(&g_grayEffect);
    ShowMagnifier(true);
    std::thread tBass2(BassThread, Cfg::kPhaseGray2Ms);
    DWORD f5 = GetTickCount();
    while ((int)(GetTickCount() - f5) < Cfg::kPhaseGray2Ms && !g_stop.load()) {
        HDC hdc = GetDC(nullptr);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        int dx = (rand() % 7) - 3;
        int dy = (rand() % 7) - 3;
        BitBlt(hdc, dx, dy, sw, sh, hdc, 0, 0, SRCCOPY);
        ReleaseDC(nullptr, hdc);
        Sleep(20);
    }
    tBass2.join();

    // ----- ФАЗА 6: 32 ошибки последовательно, заполняя экран — 10 сек -----
    SetColorEffect(&g_identityEffect);
    ShowMagnifier(false);
    ErrorsFillDesktop(Cfg::kPhaseErrorsMs);

    // ----- ФАЗА 7: Крашим explorer, показываем текст, ждём 6 сек и ребут -----
    KillExplorer();
    SleepPump(400);
    ShowFinalText();
    SleepPump(Cfg::kPhaseFinalMs);

    // ===== Шаг 8: Снимаем блокировки и перезагружаемся =====
    RemoveKbHook();
    BlockInput(FALSE);

    g_stop = true;
    if (tWatch.joinable()) tWatch.join();

    ShutdownMagnifier();

    // Реальная перезагрузка. После неё UAC снова работает штатно.
    DoReboot();

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}
