#include <windows.h>
#include <richedit.h>
#include <string>
#include <vector>

#include "serial.h"
#include "frame.h"

namespace
{
    const wchar_t* kControlClass = L"SerialChatControl";
    const wchar_t* kInputClass = L"SerialChatInput";
    const wchar_t* kOutputClass = L"SerialChatOutput";
    const wchar_t* kStatusClass = L"SerialChatStatus";
    const wchar_t* kDebugClass = L"SerialChatDebug";

    const DWORD kBaudRates[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
    const int kBaudCount = 8;
    const int kDefaultBaudIndex = 3;

    const UINT WM_APP_RX = WM_APP + 1;

    SerialPort g_serial;

    HWND g_controlWnd = nullptr;
    HWND g_inputWnd = nullptr;
    HWND g_outputWnd = nullptr;
    HWND g_statusWnd = nullptr;
    HWND g_debugWnd = nullptr;

    HWND g_comboPort = nullptr;
    HWND g_comboBaud = nullptr;
    HWND g_inputEdit = nullptr;
    HWND g_outputEdit = nullptr;
    HWND g_statusTx = nullptr;
    HWND g_statusErr = nullptr;
    HWND g_statusView = nullptr;
    HWND g_debugView = nullptr;

    std::vector<std::wstring> g_ports;
    std::wstring g_currentPort;

    frame::Receiver g_receiver;

    WNDPROC g_oldInputEditProc = nullptr;
    HFONT g_font = nullptr;
    bool g_lastWasCR = false;
    bool g_portSelected = false;

    bool g_inputLocked = false;
    bool g_outputLocked = false;
}

static void SetFont(HWND hwnd)
{
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
}

static HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, nullptr, nullptr, nullptr);
    SetFont(ctl);
    return ctl;
}

static int AliveCount()
{
    int n = 0;
    if (g_controlWnd) ++n;
    if (g_inputWnd)   ++n;
    if (g_outputWnd)  ++n;
    if (g_statusWnd)  ++n;
    if (g_debugWnd)   ++n;
    return n;
}

static bool IsLastRemaining()
{
    if (!g_inputWnd && !g_outputWnd)
        return false;

    if (AliveCount() > 1)
        return false;

    return true;
}

static bool CanCloseInput()
{
    if (!g_portSelected)
        return false;
    if (IsLastRemaining())
        return true;
    return !g_inputLocked;
}

static bool CanCloseOutput()
{
    if (!g_portSelected)
        return false;
    if (IsLastRemaining())
        return true;
    return !g_outputLocked;
}

static void UpdateCloseButtons()
{
    if (g_controlWnd) {
        HMENU menu = GetSystemMenu(g_controlWnd, FALSE);
        if (menu) {
            EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
            DrawMenuBar(g_controlWnd);
        }
    }

    if (g_inputWnd) {
        HMENU menu = GetSystemMenu(g_inputWnd, FALSE);
        if (menu) {
            UINT flags = CanCloseInput() ? MF_ENABLED : MF_GRAYED;
            EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | flags);
            DrawMenuBar(g_inputWnd);
        }
    }

    if (g_outputWnd) {
        HMENU menu = GetSystemMenu(g_outputWnd, FALSE);
        if (menu) {
            UINT flags = CanCloseOutput() ? MF_ENABLED : MF_GRAYED;
            EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | flags);
            DrawMenuBar(g_outputWnd);
        }
    }

    if (g_statusWnd) {
        HMENU menu = GetSystemMenu(g_statusWnd, FALSE);
        if (menu) {
            UINT flags = g_portSelected ? MF_ENABLED : MF_GRAYED;
            EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | flags);
            DrawMenuBar(g_statusWnd);
        }
    }
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

static void AppendToEdit(HWND edit, const wchar_t* text)
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
    }
    else if (ch == L'\n') {
        if (!g_lastWasCR)
            AppendToEdit(g_outputEdit, L"\r\n");
        g_lastWasCR = false;
    }
    else {
        wchar_t s[2] = { ch, 0 };
        AppendToEdit(g_outputEdit, s);
        g_lastWasCR = false;
    }
}

static HWND CreateFrameView(HWND parent, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        x, y, w, h, parent, nullptr, nullptr, nullptr);
    SetFont(ctl);

    LOGFONTW lf = {};
    GetObjectW(g_font, sizeof(lf), &lf);

    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);

    CHARFORMATW cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cf.yHeight = MulDiv(-lf.lfHeight, 1440, dpi);
    cf.bCharSet = lf.lfCharSet;
    lstrcpynW(cf.szFaceName, lf.lfFaceName, LF_FACESIZE);
    SendMessageW(ctl, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));

    SendMessageW(ctl, EM_EXLIMITTEXT, static_cast<WPARAM>(0x7FFFFFFE), 0);
    return ctl;
}

static void AppendRichText(HWND edit, const std::wstring& text, const std::vector<bool>& underline)
{
    if (!edit)
        return;

    int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
    SendMessageW(edit, EM_SETSEL, len, len);

    size_t i = 0;
    while (i < text.size()) {
        bool u = i < underline.size() && underline[i];
        size_t j = i;
        while (j < text.size() && (j < underline.size() && underline[j]) == u)
            ++j;

        CHARFORMATW cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_UNDERLINE;
        cf.dwEffects = u ? CFE_UNDERLINE : 0;
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));

        std::wstring run = text.substr(i, j - i);
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(run.c_str()));
        i = j;
    }

    SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
}

static std::wstring FromUtf8(const std::vector<uint8_t>& data)
{
    if (data.empty())
        return std::wstring();

    int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()), text.data(), n);
    return text;
}

static void SendFrame(wchar_t ch)
{
    if (!g_serial.isOpen())
        return;

    char buf[4];
    int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, buf, 4, nullptr, nullptr);
    if (n <= 0)
        return;

    frame::Frame f;
    f.data.assign(reinterpret_cast<uint8_t*>(buf), reinterpret_cast<uint8_t*>(buf) + n);

    frame::StuffResult s = frame::stuff(frame::serializeBody(f));
    std::vector<uint8_t> bytes = s.bytes;
    bytes.insert(bytes.end(), frame::FLAG, frame::FLAG + frame::FLAG_LEN);
    g_serial.sendBytes(bytes.data(), bytes.size());

    frame::FrameView view = frame::makeView(f, s);
    AppendRichText(g_statusView, view.before + L"\r\n", {});
    AppendRichText(g_statusView, view.after + L"\r\n", view.afterUnderline);
    AppendRichText(g_debugView, view.before + L"\r\n", {});
    AppendRichText(g_debugView, view.after + L"\r\n", view.afterUnderline);
}

static void RefreshStatus()
{
    std::wstring tx = L"Передано байт: " + std::to_wstring(g_serial.transmitted());
    SetWindowTextW(g_statusTx, tx.c_str());

    SetWindowTextW(g_statusErr, g_serial.lastError().c_str());
}

static void CheckAllWindowsClosed()
{
    if (!g_controlWnd && !g_inputWnd && !g_outputWnd && !g_statusWnd && !g_debugWnd)
        PostQuitMessage(0);
}

static void DestroyAllWindows()
{
    if (g_debugWnd)   DestroyWindow(g_debugWnd);
    if (g_outputWnd)  DestroyWindow(g_outputWnd);
    if (g_inputWnd)   DestroyWindow(g_inputWnd);
    if (g_statusWnd)  DestroyWindow(g_statusWnd);
    if (g_controlWnd) DestroyWindow(g_controlWnd);
}

static void CaretToEnd(HWND hwnd)
{
    int len = GetWindowTextLengthW(hwnd);
    SendMessageW(hwnd, EM_SETSEL, len, len);
}

static LRESULT CALLBACK InputEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CHAR: {
        if (wParam == L'\b' || wParam == 0x7F)
            return 0;

        CaretToEnd(hwnd);

        wchar_t ch = static_cast<wchar_t>(wParam);
        if (ch == L'\r' || ch >= 0x20) {
            if (g_serial.isOpen())
                SendFrame(ch);
        }
        break;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
        if (GetKeyState(VK_CONTROL) < 0) {
            if (wParam == 'V' || wParam == 'X' || wParam == 'Z')
                return 0;
        }
        switch (wParam) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_DELETE: case VK_BACK:
            CaretToEnd(hwnd);
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
        CaretToEnd(hwnd);
        SetFocus(hwnd);
        return 0;

    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_CONTEXTMENU:
        return 0;

    case WM_SETFOCUS:
        CaretToEnd(hwnd);
        break;
    }

    return CallWindowProcW(g_oldInputEditProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"COM-порт", 16, 16, 70, 20);

        g_comboPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
            16, 42, 320, 300, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_comboPort);

        CreateLabel(hwnd, L"Скорость порта (бод)", 16, 92, 140, 20);

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
        return 0;
    }

    case WM_INITMENUPOPUP:
        if (lParam == 0) {
            HMENU menu = GetSystemMenu(hwnd, FALSE);
            if (menu && !g_portSelected)
                EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
        break;

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
                g_receiver = frame::Receiver();
                EnableWindow(g_comboPort, FALSE);
                g_portSelected = true;
                UpdateCloseButtons();
                RefreshStatus();
            }
            return 0;
        }

        if (ctl == g_comboBaud && code == CBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(g_comboBaud, CB_GETCURSEL, 0, 0));
            if (sel != CB_ERR && g_serial.isOpen()) {
                g_serial.open(g_currentPort, kBaudRates[sel]);
                g_receiver = frame::Receiver();
            }
            RefreshStatus();
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        if (!g_portSelected)
            DestroyAllWindows();
        else
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (hwnd == g_controlWnd) g_controlWnd = nullptr;
        UpdateCloseButtons();
        CheckAllWindowsClosed();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"Окно ввода", 12, 12, 80, 20);

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

    case WM_INITMENUPOPUP:
        if (lParam == 0) {
            HMENU menu = GetSystemMenu(hwnd, FALSE);
            if (menu && !CanCloseInput())
                EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
        break;

    case WM_CLOSE:
        if (!CanCloseInput())
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (hwnd == g_inputWnd) g_inputWnd = nullptr;
        if (g_outputWnd && !IsLastRemaining())
            g_outputLocked = true;
        UpdateCloseButtons();
        CheckAllWindowsClosed();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"Окно вывода ", 12, 12, 90, 20);

        g_outputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
            ES_READONLY | ES_AUTOVSCROLL,
            12, 40, 460, 268, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_outputEdit);
        return 0;
    }

    case WM_APP_RX: {
        uint8_t byte = static_cast<uint8_t>(wParam);
        std::vector<frame::Frame> frames = g_receiver.feed(&byte, 1);
        for (size_t i = 0; i < frames.size(); ++i) {
            std::wstring text = FromUtf8(frames[i].data);
            for (size_t k = 0; k < text.size(); ++k)
                AppendOutputChar(text[k]);
        }
        return 0;
    }

    case WM_INITMENUPOPUP:
        if (lParam == 0) {
            HMENU menu = GetSystemMenu(hwnd, FALSE);
            if (menu && !CanCloseOutput())
                EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
        break;

    case WM_CLOSE:
        if (!CanCloseOutput())
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (hwnd == g_outputWnd) g_outputWnd = nullptr;
        if (g_inputWnd && !IsLastRemaining())
            g_inputLocked = true;
        UpdateCloseButtons();
        CheckAllWindowsClosed();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK StatusWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_statusTx = CreateLabel(hwnd, L"Передано байт: 0", 16, 16, 150, 22);
        g_statusErr = CreateLabel(hwnd, L"", 16, 40, 340, 20);

        g_statusView = CreateFrameView(hwnd, 12, 66, 396, 170);
        AppendRichText(g_statusView, frame::fieldNames() + L"\r\n", {});

        SetTimer(hwnd, 1, 1000, nullptr);
        RefreshStatus();
        return 0;
    }

    case WM_TIMER:
        if (wParam == 1)
            RefreshStatus();
        return 0;

    case WM_INITMENUPOPUP:
        if (lParam == 0) {
            HMENU menu = GetSystemMenu(hwnd, FALSE);
            if (menu && !g_portSelected)
                EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
        break;

    case WM_CLOSE:
        if (!g_portSelected)
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (hwnd == g_statusWnd) g_statusWnd = nullptr;
        g_statusView = nullptr;
        UpdateCloseButtons();
        CheckAllWindowsClosed();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"Отладочное окно", 12, 12, 140, 20);

        g_debugView = CreateFrameView(hwnd, 12, 40, 396, 200);
        AppendRichText(g_debugView, frame::fieldNames() + L"\r\n", {});
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (hwnd == g_debugWnd) g_debugWnd = nullptr;
        g_debugView = nullptr;
        CheckAllWindowsClosed();
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

    wc.lpfnWndProc = DebugWndProc;
    wc.lpszClassName = kDebugClass;
    if (!RegisterClassExW(&wc))
        return false;

    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    g_font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(nullptr, L"Не удалось загрузить Msftedit.dll.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    if (!RegisterClasses(hInstance)) {
        MessageBoxW(nullptr, L"Не удалось зарегистрировать классы окон.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    int x = 40;
    const DWORD WindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_controlWnd = CreateWindowExW(0, kControlClass, L"Управление",
        WindowStyle, x, 60, 370, 220,
        nullptr, nullptr, hInstance, nullptr);
    g_inputWnd = CreateWindowExW(0, kInputClass, L"Ввод сообщений",
        WindowStyle, x + 420, 60, 500, 330,
        nullptr, nullptr, hInstance, nullptr);
    g_outputWnd = CreateWindowExW(0, kOutputClass, L"Вывод сообщений",
        WindowStyle, x + 420, 410, 500, 370,
        nullptr, nullptr, hInstance, nullptr);
    g_statusWnd = CreateWindowExW(0, kStatusClass, L"Состояние",
        WindowStyle, x, 300, 420, 290,
        nullptr, nullptr, hInstance, nullptr);
    g_debugWnd = CreateWindowExW(0, kDebugClass, L"Отладка",
        WindowStyle, x, 610, 420, 290,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_controlWnd || !g_inputWnd || !g_outputWnd || !g_statusWnd || !g_debugWnd) {
        MessageBoxW(nullptr, L"Не удалось создать окна.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    UpdateCloseButtons();

    g_serial.setRxCallback([](uint8_t b) {
        if (g_outputWnd)
            PostMessageW(g_outputWnd, WM_APP_RX, static_cast<WPARAM>(b), 0);
        });

    ShowWindow(g_controlWnd, nCmdShow);
    UpdateWindow(g_controlWnd);
    ShowWindow(g_inputWnd, nCmdShow);
    UpdateWindow(g_inputWnd);
    ShowWindow(g_outputWnd, nCmdShow);
    UpdateWindow(g_outputWnd);
    ShowWindow(g_statusWnd, nCmdShow);
    UpdateWindow(g_statusWnd);
    ShowWindow(g_debugWnd, nCmdShow);
    UpdateWindow(g_debugWnd);

    SetForegroundWindow(g_inputWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_serial.close();
    return static_cast<int>(msg.wParam);
}