/*
 * hello.c - HELLO.EXE, the example product of Win16Setup: shows the
 * greeting chosen in Setup. HELLO.INI (written by Setup, [Ini] in
 * SETUP.INF) names the greeting file:
 *
 *   [Hello]
 *   Greeting=C:\HELLO\GREET\EN.INI
 */
#include <windows.h>
#include <string.h>

static char text[128];

LRESULT CALLBACK __export WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT r;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &r);
        SetBkMode(dc, TRANSPARENT);
        DrawText(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char ini[160], greet[160], *p;
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;
    (void)cmd;
    if (prev)
        return 0;
    GetModuleFileName(inst, ini, sizeof(ini));
    p = strrchr(ini, '\\');
    lstrcpy(p ? p + 1 : ini, "HELLO.INI");
    GetPrivateProfileString("Hello", "Greeting", "", greet, sizeof(greet), ini);
    GetPrivateProfileString("Greeting", "Text", "Hello!", text, sizeof(text), greet);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "Hello";
    RegisterClass(&wc);
    hwnd = CreateWindow("Hello", "Hello", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, 240, 120, NULL, NULL, inst,
                        NULL);
    ShowWindow(hwnd, show);
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
