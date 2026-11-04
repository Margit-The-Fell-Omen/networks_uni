#define NOMINMAX
#include <windows.h>
#include <richedit.h>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "serial.h"
#include "frame.h"
#include "channel.h"

namespace
{
    const wchar_t* kControlClass = L"SerialChatControl";
    const wchar_t* kInputClass = L"SerialChatInput";
    const wchar_t* kOutputClass = L"SerialChatOutput";
    const wchar_t* kStatusClass = L"SerialChatStatus";

    const DWORD kBaudRates[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
    const int kBaudCount = 8;
    const int kDefaultBaudIndex = 3;

    const UINT WM_APP_RX = WM_APP + 1;
    const UINT_PTR kEngineTimer = 1;
    const UINT kEngineIntervalMs = 10;

    SerialPort g_serial;
    channel::Session g_session;

    struct TxJob
    {
        std::vector<uint8_t> bytes;
        frame::FrameView view;
        channel::Config config;
    };

    std::deque<TxJob> g_txQueue;

    HWND g_controlWnd = nullptr;
    HWND g_inputWnd = nullptr;
    HWND g_outputWnd = nullptr;
    HWND g_statusWnd = nullptr;

    HWND g_comboPort = nullptr;
    HWND g_comboBaud = nullptr;
    HWND g_checkErrors = nullptr;
    HWND g_checkMultiAccess = nullptr;
    bool g_errorsEnabled = true;        // состояние чекбокса «Эмуляция ошибок»
    bool g_multiAccessEnabled = true;   // состояние чекбокса «Эмуляция множественного доступа»
    HWND g_inputEdit = nullptr;
    HWND g_outputEdit = nullptr;
    HWND g_statusTx = nullptr;
    HWND g_statusState = nullptr;
    HWND g_statusStarsTitle = nullptr;
    HWND g_statusStars = nullptr;
    HWND g_statusCounters = nullptr;
    HWND g_statusHead = nullptr;
    HWND g_statusView = nullptr;

    std::vector<std::wstring> g_ports;
    std::wstring g_currentPort;
    std::wstring g_pendingText;
    DWORD g_currentBaud = 9600;

    frame::Receiver g_receiver;

    const COLORREF kTxColor = RGB(0, 0, 0);
    const COLORREF kRxColor = RGB(0, 0, 200);

    WNDPROC g_oldInputEditProc = nullptr;
    HFONT g_font = nullptr;
    HFONT g_boldFont = nullptr;
    bool g_lastWasCR = false;
    bool g_portSelected = false;

    bool g_inputLocked = false;
    bool g_outputLocked = false;

    // Состояние, которое читает окно «Состояние».
    std::wstring g_stars;
    std::wstring g_lastEvent;
    int g_attempts = 0;
    int g_collisions = 0;
    int g_busyWaits = 0;
    channel::State g_lastState = channel::State::Idle;
    std::deque<frame::FrameView> g_pendingViews;
    COLORREF g_stateColor = RGB(0, 0, 0);
    COLORREF g_starsColor = RGB(128, 128, 128);
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

static HWND CreateLabelRight(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        x, y, w, h, parent, nullptr, nullptr, nullptr);
    SetFont(ctl);
    return ctl;
}

static HWND CreateStatusText(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(0, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_LEFT,
        x, y, w, h, parent, nullptr, nullptr, nullptr);
    SetFont(ctl);
    return ctl;
}

static void SetLabel(HWND hwnd, const std::wstring& text)
{
    if (!hwnd)
        return;

    const int len = GetWindowTextLengthW(hwnd);
    std::wstring current(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, current.data(), len + 1);
    current.resize(static_cast<size_t>(len));

    if (current != text)
        SetWindowTextW(hwnd, text.c_str());
}

static std::wstring StateText(channel::State state)
{
    switch (state)
    {
    case channel::State::Idle:         return L"ожидание";
    case channel::State::Done:         return L"свободен (кадр передан)";
    case channel::State::Error:        return L"ошибка";
    default:                           return channel::stateName(state);
    }
}

static COLORREF StateColor(channel::State state)
{
    switch (state)
    {
    case channel::State::Busy:         return RGB(180, 100, 0);
    case channel::State::Transmitting: return RGB(0, 90, 170);
    case channel::State::Collision:
    case channel::State::Jam:
    case channel::State::Error:        return RGB(200, 0, 0);
    case channel::State::Backoff:      return RGB(150, 0, 150);
    case channel::State::Done:         return RGB(0, 128, 0);
    default:                           return RGB(0, 0, 0);
    }
}

static void OnChannelState(channel::State state)
{
    g_lastState = state;

    switch (state)
    {
    case channel::State::Listening:
        g_lastEvent = L"Прослушивание канала";
        break;
    case channel::State::Busy:
        ++g_busyWaits;
        g_lastEvent = L"Канал занят, ожидание";
        break;
    case channel::State::Transmitting:
        ++g_attempts;
        g_lastEvent = L"Передача кадра";
        break;
    case channel::State::Collision:
    case channel::State::Jam:
    case channel::State::Backoff:
        break;
    case channel::State::Done:
        g_lastEvent = L"Кадр передан";
        break;
    case channel::State::Error:
        g_lastEvent = L"Кадр не передан";
        break;
    case channel::State::Cancelled:
        g_lastEvent = L"Передача отменена";
        break;
    default:
        break;
    }
}

static void OnChannelCollision(int byteIndex, int attempt)
{
    ++g_collisions;
    if (!g_stars.empty())
        g_stars += L' ';
    g_stars += L'*';

    wchar_t buf[128];
    wsprintfW(buf, L"Коллизия на байте %d (попытка %d)", byteIndex + 1, attempt);
    g_lastEvent = buf;
}

static void OnChannelBackoff(int delayMs)
{
    if (!g_lastEvent.empty())
        g_lastEvent += L", задержка " + std::to_wstring(delayMs) + L" мс";
    else
        g_lastEvent = L"Задержка " + std::to_wstring(delayMs) + L" мс";
}

static void OnSessionFinished()
{
    const channel::Result& result = g_session.result();

    g_attempts = result.attempts;
    g_collisions = result.collisions;
    g_busyWaits = result.busyWaits;

    if (result.cancelled)
    {
        g_lastState = channel::State::Cancelled;
        g_lastEvent = L"Передача отменена";
    }
    else if (result.success)
    {
        g_lastState = channel::State::Done;
        g_lastEvent = L"Кадр передан";
    }
    else
    {
        g_lastState = channel::State::Error;
        g_lastEvent = result.mediumError ? L"Ошибка обмена с портом" : L"Исчерпаны попытки передачи";
    }
}

static int AliveCount()
{
    int n = 0;
    if (g_controlWnd) ++n;
    if (g_inputWnd)   ++n;
    if (g_outputWnd)  ++n;
    if (g_statusWnd)  ++n;
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

static HWND CreateFrameView(HWND parent, int x, int y, int w, int h, bool scroll, bool horizontalOnly = false)
{
    DWORD style = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY;
    if (scroll)
        style |= WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL | ES_AUTOHSCROLL;
    if (horizontalOnly)
        style |= WS_HSCROLL | ES_AUTOHSCROLL;

    HWND ctl = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
        style, x, y, w, h, parent, nullptr, nullptr, nullptr);
    SetFont(ctl);

    CHARFORMATW cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cf.yHeight = 10 * 20;
    cf.bCharSet = DEFAULT_CHARSET;
    lstrcpynW(cf.szFaceName, L"Consolas", LF_FACESIZE);
    SendMessageW(ctl, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));

    SendMessageW(ctl, EM_EXLIMITTEXT, static_cast<WPARAM>(0x7FFFFFFE), 0);
    SendMessageW(ctl, EM_SETTARGETDEVICE, 0, 0);
    return ctl;
}

static void AppendRichText(HWND edit, const std::wstring& text, const std::vector<bool>& underline, COLORREF color)
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
        cf.dwMask = CFM_UNDERLINE | CFM_COLOR;
        cf.dwEffects = u ? CFE_UNDERLINE : 0;
        cf.crTextColor = color;
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));

        std::wstring run = text.substr(i, j - i);
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(run.c_str()));
        i = j;
    }

    SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(edit, EM_LINESCROLL, static_cast<WPARAM>(-32768), 0);
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

static void AppendTxFrameView(HWND view, const frame::FrameView& fv, COLORREF color)
{
    std::wstring text;
    std::vector<bool> mask;

    text += fv.before + L"\r\n";
    mask.insert(mask.end(), fv.before.size(), false);
    mask.push_back(false);
    mask.push_back(false);

    text += fv.after + L"\r\n";
    mask.insert(mask.end(), fv.afterUnderline.begin(), fv.afterUnderline.end());
    mask.push_back(false);
    mask.push_back(false);

    if (fv.hasErrorRow) {
        text += fv.afterError + L"\r\n";
        mask.insert(mask.end(), fv.errorUnderline.begin(), fv.errorUnderline.end());
        mask.push_back(false);
        mask.push_back(false);
    }

    AppendRichText(view, text, mask, color);
}

static void AppendRxFrameView(HWND view, const frame::FrameView& fv, COLORREF color)
{
    std::wstring text;
    std::vector<bool> mask;

    if (fv.hasErrorRow) {
        text += fv.afterError + L"\r\n";
        mask.insert(mask.end(), fv.errorUnderline.begin(), fv.errorUnderline.end());
        mask.push_back(false);
        mask.push_back(false);
    }

    text += fv.before + L"\r\n";
    mask.insert(mask.end(), fv.before.size(), false);
    mask.push_back(false);
    mask.push_back(false);

    text += fv.after + L"\r\n";
    mask.insert(mask.end(), fv.afterUnderline.begin(), fv.afterUnderline.end());
    mask.push_back(false);
    mask.push_back(false);

    AppendRichText(view, text, mask, color);
}

static void SendFrameData(const std::vector<uint8_t>& data)
{
    if (!g_serial.isOpen())
        return;

    frame::Frame f;
    f.data = data;
    crc::setFCS(f);                                   // FCS считается только по полю данных
    frame::StuffResult clean = frame::stuff(frame::serializeBody(f));   // стаффинг - над кадром с готовым FCS

    // эмуляция ошибки непосредственно перед передачей
    frame::Frame sent = f;
    frame::ErrorInfo err;
    if (g_errorsEnabled)
        err = frame::injectError(sent, frame::ERROR_PERCENT);

    frame::StuffResult s = err.applied ? frame::stuff(frame::serializeBody(sent)) : clean;

    TxJob job;
    job.bytes = s.bytes;
    job.bytes.insert(job.bytes.end(), frame::FLAG, frame::FLAG + frame::FLAG_LEN);
    job.view = frame::makeView(f, clean, sent, s, err);
    job.config.enabled = g_multiAccessEnabled;
    // Порция зависит от скорости порта, чтобы окно не замирало на медленном канале.
    job.config.chunkBytes = std::max(1, std::min(16, static_cast<int>(g_currentBaud / 1000)));
    g_txQueue.push_back(std::move(job));
}

static void SendLine(const std::wstring& line)
{
    std::wstring text = line + L"\r";
    std::vector<uint8_t> current;

    for (size_t i = 0; i < text.size(); i++) {
        char buf[4];
        int n = WideCharToMultiByte(CP_UTF8, 0, &text[i], 1, buf, 4, nullptr, nullptr);
        if (n <= 0)
            continue;

        if (current.size() + n > frame::MAX_DATA_LEN) {
            SendFrameData(current);
            current.clear();
        }
        current.insert(current.end(), reinterpret_cast<uint8_t*>(buf), reinterpret_cast<uint8_t*>(buf) + n);
    }

    if (!current.empty())
        SendFrameData(current);
}

static void RefreshStatus()
{
    if (g_statusTx)
        SetLabel(g_statusTx, L"Передано байт: " + std::to_wstring(g_serial.transmitted()));

    if (g_statusState)
    {
        std::wstring text = L"Состояние канала: ";
        COLORREF color = StateColor(g_lastState);

        if (!g_multiAccessEnabled && !g_session.running())
        {
            text += L"эмуляция выключена";
            color = RGB(110, 110, 110);
        }
        else
        {
            text += StateText(g_lastState);
        }

        SetLabel(g_statusState, text);
        if (color != g_stateColor)
        {
            g_stateColor = color;
            InvalidateRect(g_statusState, nullptr, TRUE);
        }
    }

    if (g_statusStars)
    {
        SetLabel(g_statusStars, g_stars.empty() ? L"—" : g_stars);

        const COLORREF color = g_stars.empty() ? RGB(128, 128, 128) : RGB(200, 0, 0);
        if (color != g_starsColor)
        {
            g_starsColor = color;
            InvalidateRect(g_statusStars, nullptr, TRUE);
        }
    }

    if (g_statusCounters)
    {
        wchar_t buf[512];
        wsprintfW(buf, L"Попытки: %d   Коллизий: %d   Ожиданий канала: %d",
                  g_attempts, g_collisions, g_busyWaits);
        std::wstring counters = buf;
        if (!g_lastEvent.empty())
            counters += L"   |   " + g_lastEvent;
        SetLabel(g_statusCounters, counters);
    }

    if (g_statusView)
    {
        if (!g_pendingViews.empty())
        {
            for (const frame::FrameView& view : g_pendingViews)
                AppendTxFrameView(g_statusView, view, kTxColor);
            g_pendingViews.clear();
        }
    }
    else
    {
        g_pendingViews.clear();
    }
}

static void StartNextFrame()
{
    if (g_session.running() || g_txQueue.empty())
        return;

    TxJob job = std::move(g_txQueue.front());
    g_txQueue.pop_front();

    g_stars.clear();
    g_attempts = 0;
    g_collisions = 0;
    g_busyWaits = 0;
    g_lastState = channel::State::Listening;
    g_lastEvent = L"Кадр подготовлен к передаче";
    g_pendingViews.push_back(std::move(job.view));

    g_session.start(std::move(job.bytes), job.config);
}

static void EngineTick()
{
    bool justStarted = false;

    if (!g_session.running() && !g_txQueue.empty())
    {
        StartNextFrame();
        justStarted = true;
    }

    if (g_session.running() && !justStarted)
    {
        if (!g_session.tick())
            OnSessionFinished();
    }

    RefreshStatus();
}

static void CheckAllWindowsClosed()
{
    if (!g_controlWnd && !g_inputWnd && !g_outputWnd && !g_statusWnd)
        PostQuitMessage(0);
}

static void DestroyAllWindows()
{
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
        if (ch == L'\r') {
            SendLine(g_pendingText);
            g_pendingText.clear();
        }
        else if (ch >= 0x20) {
            g_pendingText += ch;
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

        CreateLabel(hwnd, L"Скорость порта (бод)", 16, 92, 160, 20);

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

        wchar_t errText[64];
        wsprintfW(errText, L"Эмуляция ошибок (вероятность %d %%)", frame::ERROR_PERCENT);
        g_checkErrors = CreateWindowExW(0, L"BUTTON", errText,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            16, 158, 330, 22, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_checkErrors);
        SendMessageW(g_checkErrors, BM_SETCHECK, BST_CHECKED, 0);

        HWND group = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            8, 184, 344, 156, hwnd, nullptr, nullptr, nullptr);
        SetFont(group);

        g_checkMultiAccess = CreateWindowExW(0, L"BUTTON", L"Эмуляция множественного доступа",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            20, 210, 320, 22, hwnd, nullptr, nullptr, nullptr);
        SetFont(g_checkMultiAccess);
        SendMessageW(g_checkMultiAccess, BM_SETCHECK, BST_CHECKED, 0);

        wchar_t buf[128];
        wsprintfW(buf, L"Занятость канала: %d %%", channel::BUSY_PERCENT);
        CreateLabel(hwnd, buf, 40, 238, 296, 18);

        wsprintfW(buf, L"Коллизия: %d %%", channel::COLLISION_PERCENT);
        CreateLabel(hwnd, buf, 40, 258, 296, 18);

        CreateLabel(hwnd, L"Коллизия: в отношении байта", 40, 278, 296, 18);

        wsprintfW(buf, L"Jam-сигнал: %d байта 0x%02X", channel::JAM_BYTES, channel::JAM_VALUE);
        CreateLabel(hwnd, buf, 40, 298, 296, 18);

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
                int baudSel = static_cast<int>(SendMessageW(g_comboBaud, CB_GETCURSEL, 0, 0));
                if (baudSel == CB_ERR)
                    baudSel = kDefaultBaudIndex;

                const std::wstring port = g_ports[sel];
                const DWORD baud = kBaudRates[baudSel];

                if (!g_serial.open(port, baud)) {
                    std::wstring error = g_serial.lastError();
                    if (error.empty())
                        error = L"Не удалось открыть выбранный COM-порт.";
                    MessageBoxW(hwnd, error.c_str(), L"Ошибка открытия порта", MB_ICONERROR);
                    return 0;
                }

                g_currentPort = port;
                g_currentBaud = baud;
                g_receiver = frame::Receiver();
                g_pendingText.clear();
                g_txQueue.clear();

                // Номер порта и скорость выбираются один раз при инициализации.
                EnableWindow(g_comboPort, FALSE);
                EnableWindow(g_comboBaud, FALSE);
                g_portSelected = true;
                UpdateCloseButtons();
                RefreshStatus();
            }
            return 0;
        }

        if (ctl == g_checkErrors && code == BN_CLICKED) {
            g_errorsEnabled = SendMessageW(g_checkErrors, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        }

        if (ctl == g_checkMultiAccess && code == BN_CLICKED) {
            g_multiAccessEnabled = SendMessageW(g_checkMultiAccess, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
        if (hwnd == g_controlWnd) {
            g_controlWnd = nullptr;
            g_comboPort = nullptr;
            g_comboBaud = nullptr;
            g_checkErrors = nullptr;
            g_checkMultiAccess = nullptr;
        }
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

            frame::Frame raw = frames[i];
            crc::IsCorrect(frames[i]);

            frame::ErrorInfo err;
            for (size_t b = 0; b < raw.data.size(); ++b) {
                if (raw.data[b] != frames[i].data[b]) {
                    uint8_t diff = raw.data[b] ^ frames[i].data[b];
                    int bit = 7;
                   while (bit >= 0 && !((diff >> bit) & 1)) --bit;
                    err.applied = true;
                    err.byteIndex = b;
                    err.bitIndex = 7 - bit;
                    break;
                }
            }

            if (g_statusView) {
                frame::StuffResult fixS = frame::stuff(frame::serializeBody(frames[i]));
                frame::StuffResult rawS = frame::stuff(frame::serializeBody(raw));
                AppendRxFrameView(g_statusView,
                    frame::makeView(frames[i], fixS, raw, rawS, err), kRxColor);
            }

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
        RECT rc = {};
        GetClientRect(hwnd, &rc);

        const bool wide = rc.right >= 900;

        g_statusTx = CreateLabel(hwnd, L"Передано байт: 0", 16, 4, 300, 18);

        int starsTitleY = 24;
        int starsY = 22;
        int countersY = 46;
        int headY = 68;

        if (wide) {
            g_statusState = CreateStatusText(hwnd, L"Состояние канала: ожидание",
                330, 4, rc.right - 346, 18);
        } else {
            g_statusState = CreateStatusText(hwnd, L"Состояние канала: ожидание",
                16, 24, rc.right - 32, 18);
            starsTitleY = 44;
            starsY = 42;
            countersY = 70;
            headY = 92;
        }

        const int viewY = headY + 28;

        g_statusStarsTitle = CreateLabel(hwnd, L"Коллизии последнего кадра:",
            16, starsTitleY, 230, 18);

        g_statusStars = CreateWindowExW(0, L"STATIC", L"—",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            250, starsY, rc.right - 266, 24, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(g_statusStars, WM_SETFONT,
            reinterpret_cast<WPARAM>(g_boldFont ? g_boldFont : g_font), TRUE);

        g_statusCounters = CreateLabel(hwnd,
            L"Попытки: 0   Коллизий: 0   Ожиданий канала: 0",
            16, countersY, rc.right - 32, 18);

        g_statusHead = CreateFrameView(hwnd, 12, headY, rc.right - 24, 26, false);
        AppendRichText(g_statusHead, frame::fieldNames(), {}, kTxColor);
        // Заголовок полей показываем с начала строки, а не с конца.
        SendMessageW(g_statusHead, EM_SETSEL, 0, 0);
        SendMessageW(g_statusHead, EM_SCROLLCARET, 0, 0);

        g_statusView = CreateFrameView(hwnd, 12, viewY, rc.right - 24,
            rc.bottom - viewY - 8, true);

        RefreshStatus();
        SetTimer(hwnd, kEngineTimer, kEngineIntervalMs, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wParam == kEngineTimer)
            EngineTick();
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);

        if (child == g_statusStars)
            SetTextColor(hdc, g_starsColor);
        else
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));

        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

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
        KillTimer(hwnd, kEngineTimer);
        if (hwnd == g_statusWnd) g_statusWnd = nullptr;
        g_statusTx = nullptr;
        g_statusState = nullptr;
        g_statusStarsTitle = nullptr;
        g_statusStars = nullptr;
        g_statusCounters = nullptr;
        g_statusHead = nullptr;
        g_statusView = nullptr;
        g_pendingViews.clear();
        UpdateCloseButtons();
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

    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    g_font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    LOGFONTW lf = {};
    if (GetObjectW(g_font, sizeof(lf), &lf)) {
        lf.lfWeight = FW_BOLD;
        lf.lfHeight = -18;
        lstrcpynW(lf.lfFaceName, L"Consolas", LF_FACESIZE);
        g_boldFont = CreateFontIndirectW(&lf);
    }
    if (!g_boldFont)
        g_boldFont = g_font;

    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(nullptr, L"Не удалось загрузить Msftedit.dll.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    if (!RegisterClasses(hInstance)) {
        MessageBoxW(nullptr, L"Не удалось зарегистрировать классы окон.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    g_session.setSend([](const uint8_t *data, size_t size) {
        return g_serial.sendBytes(data, size);
        });
    g_session.setOnState([](channel::State state) {
        OnChannelState(state);
        });
    g_session.setOnCollision([](int byteIndex, int attempt) {
        OnChannelCollision(byteIndex, attempt);
        });
    g_session.setOnBackoff([](int delayMs) {
        OnChannelBackoff(delayMs);
        });

    int x = 40;
    const DWORD WindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_controlWnd = CreateWindowExW(0, kControlClass, L"Управление",
        WindowStyle, x, 60, 370, 400,
        nullptr, nullptr, hInstance, nullptr);
    g_inputWnd = CreateWindowExW(0, kInputClass, L"Ввод сообщений",
        WindowStyle, x + 420, 60, 500, 330,
        nullptr, nullptr, hInstance, nullptr);
    g_outputWnd = CreateWindowExW(0, kOutputClass, L"Вывод сообщений",
        WindowStyle, x + 420, 410, 500, 370,
        nullptr, nullptr, hInstance, nullptr);
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    RECT work = { 0, 0, screenW, GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    // Широкое окно состояния — внизу, как в предыдущих лабораторных.
    int statusX = x;
    int statusY = 790;
    int statusW = screenW - 80;
    int statusH = work.bottom - statusY - 20;
    if (statusH < 200) {
        statusY = 610;
        statusW = 420;
        statusH = 290;
    }

    g_statusWnd = CreateWindowExW(0, kStatusClass, L"Состояние",
        WindowStyle, statusX, statusY, statusW, statusH,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_controlWnd || !g_inputWnd || !g_outputWnd || !g_statusWnd) {
        MessageBoxW(nullptr, L"Не удалось создать окна.", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    UpdateCloseButtons();

    // TEMP-DEMO: проверить отображение коллизий без COM-порта.
    g_stars = L"* * *";
    g_attempts = 2;
    g_collisions = 3;
    g_busyWaits = 1;
    g_lastState = channel::State::Backoff;
    g_lastEvent = L"Коллизия на байте 12 (попытка 2), задержка 50 мс";

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

    SetForegroundWindow(g_inputWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_session.cancel();
    g_serial.close();

    if (g_boldFont && g_boldFont != g_font)
        DeleteObject(g_boldFont);

    return static_cast<int>(msg.wParam);
}