#include <windows.h>
#include <string>
#include <vector>

#include "serial.h"

namespace
{
    const wchar_t *kControlClass = L"SerialChatControl";
    const wchar_t *kInputClass = L"SerialChatInput";
    const wchar_t *kOutputClass = L"SerialChatOutput";
    const wchar_t *kStatusClass = L"SerialChatStatus";

    const DWORD kBaudRates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    const int kBaudCount = 8;
    const int kDefaultBaudIndex = 3;

    const UINT WM_APP_RX = WM_APP + 1;

    SerialPort g_serial;

    HWND g_controlWnd = nullptr;
    HWND g_inputWnd = nullptr;
    HWND g_outputWnd = nullptr;
    HWND g_statusWnd = nullptr;

    HWND g_comboPort = nullptr;
    HWND g_comboBaud = nullptr;
    HWND g_inputEdit = nullptr;
    HWND g_outputEdit = nullptr;
    HWND g_statusConn = nullptr;
    HWND g_statusTx = nullptr;
    HWND g_statusErr = nullptr;

    std::vector<std::wstring> g_ports;
    std::wstring g_currentPort;

    WNDPROC g_oldInputEditProc = nullptr;
    HFONT g_font = nullptr;
    bool g_lastWasCR = false;
}

static void SetFont(HWND hwnd)
{
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
}

static HWND CreateLabel(HWND parent, const wchar_t *text, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(0, L"STATIC", text,
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               x, y, w, h, parent, nullptr, nullptr, nullptr);
    SetFont(ctl);
    return ctl;
}

static void FillPortCombo()
{
    wchar_t current[64] = L"";
    GetWindowTextW(g_comboPort, current, 64);

    SendMessageW(g_comboPort, CB_RESETCONTENT, 0, 0);
    g_ports = g_serial.enumeratePorts();

    int select = -1;
    for (size_t i = 0; i < g_ports.size(); ++i) {
        SendMessageW(g_comboPort, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(g_ports[i].c_str()));
        if (current[0] && g_ports[i] == current)
            select = static_cast<int>(i);
    }
    SendMessageW(g_comboPort, CB_SETCURSEL, select, 0);
}

static void AppendToEdit(HWND edit, const wchar_t *text)
{
    int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));
    SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

static void AppendOutputChar(wchar_t ch)
{
    if (ch == L'\r') {
        AppendToEdit(g_outputEdit, L"\r\n");
        g_lastWasCR = true;
    } else if (ch == L'\n') {
        if (!g_lastWasCR)
            AppendToEdit(g_outputEdit, L"\r\n");
        g_lastWasCR = false;
    } else {
        wchar_t s[2] = {ch, 0};
        AppendToEdit(g_outputEdit, s);
        g_lastWasCR = false;
    }
}

static void RefreshStatus()
{
    std::wstring conn = g_serial.isOpen() ? L"Подключено" : L"Отключено";
    SetWindowTextW(g_statusConn, conn.c_str());

    std::wstring tx = L"Передано символов: " + std::to_wstring(g_serial.transmitted());
    SetWindowTextW(g_statusTx, tx.c_str());

    SetWindowTextW(g_statusErr, g_serial.lastError().c_str());
}

static LRESULT CALLBACK InputEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CHAR) {
        wchar_t ch = static_cast<wchar_t>(wParam);
        if (ch == L'\r' || ch >= 0x20) {
            if (g_serial.isOpen())
                g_serial.send(ch);
        }
    }
    return CallWindowProcW(g_oldInputEditProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"COM-порт", 16, 16, 200, 20);

        g_comboPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                      16, 42, 320, 300, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_comboPort);

        CreateLabel(hwnd, L"Скорость порта (бод)", 16, 92, 220, 20);

        g_comboBaud = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                      16, 118, 320, 300, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_comboBaud);

        for (int i = 0; i < kBaudCount; ++i) {
            wchar_t buf[32];
            wsprintfW(buf, L"%lu", kBaudRates[i]);
            SendMessageW(g_comboBaud, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
        }
        SendMessageW(g_comboBaud, CB_SETCURSEL, kDefaultBaudIndex, 0);

        FillPortCombo();
        SetTimer(hwnd, 2, 2000, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wParam == 2 && !g_serial.isOpen())
            FillPortCombo();
        return 0;

    case WM_COMMAND: {
        int code = HIWORD(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);

        if (ctl == g_comboPort && code == CBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(g_comboPort, CB_GETCURSEL, 0, 0));
            if (sel != CB_ERR && sel < static_cast<int>(g_ports.size())) {
                g_currentPort = g_ports[sel];
                int baudSel = static_cast<int>(SendMessageW(g_comboBaud, CB_GETCURSEL, 0, 0));
                if (baudSel == CB_ERR)
                    baudSel = kDefaultBaudIndex;
                g_serial.open(g_currentPort, kBaudRates[baudSel]);
                RefreshStatus();
            }
            return 0;
        }

        if (ctl == g_comboBaud && code == CBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(g_comboBaud, CB_GETCURSEL, 0, 0));
            if (sel != CB_ERR && g_serial.isOpen())
                g_serial.open(g_currentPort, kBaudRates[sel]);
            RefreshStatus();
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"Окно ввода (передача)", 12, 12, 440, 20);

        g_inputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                          ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                      12, 40, 460, 230, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_inputEdit);
        g_oldInputEditProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_inputEdit, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(InputEditProc)));
        SetFocus(g_inputEdit);
        return 0;
    }

    case WM_SETFOCUS:
        SetFocus(g_inputEdit);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"Окно вывода (приём)", 12, 12, 440, 20);

        g_outputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                           ES_READONLY | ES_AUTOVSCROLL,
                                       12, 40, 460, 268, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_outputEdit);
        return 0;
    }

    case WM_APP_RX:
        AppendOutputChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK StatusWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_statusConn = CreateLabel(hwnd, L"Отключено", 16, 16, 340, 22);
        g_statusTx = CreateLabel(hwnd, L"Передано символов: 0", 16, 48, 340, 22);
        g_statusErr = CreateLabel(hwnd, L"", 16, 80, 340, 60);

        SetTimer(hwnd, 1, 1000, nullptr);
        RefreshStatus();
        return 0;
    }

    case WM_TIMER:
        if (wParam == 1)
            RefreshStatus();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool RegisterClasses(HINSTANCE inst)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    wc.lpfnWndProc = ControlWndProc;
    wc.lpszClassName = kControlClass;
    if (!RegisterClassExW(&wc))
        return false;

    wc.lpfnWndProc = InputWndProc;
    wc.lpszClassName = kInputClass;
    if (!RegisterClassExW(&wc))
        return false;

    wc.lpfnWndProc = OutputWndProc;
    wc.lpszClassName = kOutputClass;
    if (!RegisterClassExW(&wc))
        return false;

    wc.lpfnWndProc = StatusWndProc;
    wc.lpszClassName = kStatusClass;
    if (!RegisterClassExW(&wc))
        return false;

    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    g_font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    if (!RegisterClasses(hInstance)) {
        MessageBoxW(nullptr, L"Не удалось зарегистрировать классы окон.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    int x = 40;
    g_controlWnd = CreateWindowExW(0, kControlClass, L"Управление",
                                   WS_OVERLAPPEDWINDOW, x, 60, 380, 240,
                                   nullptr, nullptr, hInstance, nullptr);
    g_inputWnd = CreateWindowExW(0, kInputClass, L"Ввод сообщений",
                                 WS_OVERLAPPEDWINDOW, x + 420, 60, 500, 320,
                                 nullptr, nullptr, hInstance, nullptr);
    g_outputWnd = CreateWindowExW(0, kOutputClass, L"Вывод сообщений",
                                  WS_OVERLAPPEDWINDOW, x + 420, 410, 500, 360,
                                  nullptr, nullptr, hInstance, nullptr);
    g_statusWnd = CreateWindowExW(0, kStatusClass, L"Состояние",
                                  WS_OVERLAPPEDWINDOW, x, 330, 420, 200,
                                  nullptr, nullptr, hInstance, nullptr);

    if (!g_controlWnd || !g_inputWnd || !g_outputWnd || !g_statusWnd) {
        MessageBoxW(nullptr, L"Не удалось создать окна.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    g_serial.setRxCallback([](wchar_t ch) {
        if (g_outputWnd)
            PostMessageW(g_outputWnd, WM_APP_RX, static_cast<WPARAM>(ch), 0);
    });

    ShowWindow(g_controlWnd, nCmdShow);
    UpdateWindow(g_controlWnd);
    ShowWindow(g_inputWnd, nCmdShow);
    UpdateWindow(g_inputWnd);
    ShowWindow(g_outputWnd, nCmdShow);
    UpdateWindow(g_outputWnd);
    ShowWindow(g_statusWnd, nCmdShow);
    UpdateWindow(g_statusWnd);

    SetForegroundWindow(g_inputWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_serial.close();
    return static_cast<int>(msg.wParam);
}
