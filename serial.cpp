#include "serial.h"

namespace
{
    int utf8Encode(wchar_t ch, char *out)
    {
        unsigned int cp = static_cast<unsigned int>(ch);
        if (cp < 0x80) {
            out[0] = static_cast<char>(cp);
            return 1;
        }
        if (cp < 0x800) {
            out[0] = static_cast<char>(0xC0 | (cp >> 6));
            out[1] = static_cast<char>(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp < 0x10000) {
            out[0] = static_cast<char>(0xE0 | (cp >> 12));
            out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[2] = static_cast<char>(0x80 | (cp & 0x3F));
            return 3;
        }
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }

    class Utf8Decoder
    {
    public:
        bool push(unsigned char b, wchar_t &out)
        {
            if (m_len == 0) {
                if (b < 0x80) {
                    out = static_cast<wchar_t>(b);
                    return true;
                }
                if ((b & 0xE0) == 0xC0) { m_need = 2; }
                else if ((b & 0xF0) == 0xE0) { m_need = 3; }
                else if ((b & 0xF8) == 0xF0) { m_need = 4; }
                else { out = 0xFFFD; return true; }
                m_buf[0] = b;
                m_len = 1;
                return false;
            }

            m_buf[m_len++] = b;
            if (m_len < m_need)
                return false;

            if (m_need == 2)
                out = static_cast<wchar_t>(((m_buf[0] & 0x1F) << 6) | (m_buf[1] & 0x3F));
            else if (m_need == 3)
                out = static_cast<wchar_t>(((m_buf[0] & 0x0F) << 12) | ((m_buf[1] & 0x3F) << 6) | (m_buf[2] & 0x3F));
            else
                out = static_cast<wchar_t>(((m_buf[0] & 0x07) << 18) | ((m_buf[1] & 0x3F) << 12) | ((m_buf[2] & 0x3F) << 6) | (m_buf[3] & 0x3F));

            m_len = 0;
            m_need = 0;
            return true;
        }

    private:
        unsigned char m_buf[4] = {0, 0, 0, 0};
        int m_len = 0;
        int m_need = 0;
    };
}

std::vector<std::wstring> SerialPort::enumeratePorts()
{
    std::vector<std::wstring> result;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return result;

    wchar_t valueName[256];
    BYTE valueData[256];

    for (DWORD i = 0;; ++i) {
        DWORD valueNameSize = 256;
        DWORD valueDataSize = sizeof(valueData);
        DWORD type = 0;

        LONG r = RegEnumValueW(hKey, i, valueName, &valueNameSize,
                               nullptr, &type, valueData, &valueDataSize);
        if (r == ERROR_NO_MORE_ITEMS)
            break;
        if (r != ERROR_SUCCESS || type != REG_SZ)
            continue;

        result.emplace_back(reinterpret_cast<wchar_t *>(valueData));
    }

    RegCloseKey(hKey);
    return result;
}

bool SerialPort::open(const std::wstring &port, DWORD baud)
{
    close();

    std::wstring path = L"\\\\.\\" + port;

    m_h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_h == INVALID_HANDLE_VALUE) {
        m_lastError = L"Не удалось открыть " + port + L" (код " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_h, &dcb)) {
        m_lastError = L"Не удалось прочитать параметры порта";
        close();
        return false;
    }

    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;

    if (!SetCommState(m_h, &dcb)) {
        m_lastError = L"Не удалось установить параметры порта";
        close();
        return false;
    }

    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 100;
    to.ReadTotalTimeoutMultiplier = 10;
    to.WriteTotalTimeoutConstant = 50;
    to.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(m_h, &to);

    PurgeComm(m_h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    m_lastError.clear();
    m_running = true;
    m_rx = std::thread(&SerialPort::rxLoop, this);
    return true;
}

void SerialPort::close()
{
    m_running = false;
    if (m_rx.joinable())
        m_rx.join();

    if (m_h != INVALID_HANDLE_VALUE) {
        CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }
}

bool SerialPort::send(wchar_t ch)
{
    if (m_h == INVALID_HANDLE_VALUE)
        return false;

    char buf[4];
    int n = utf8Encode(ch, buf);

    DWORD written = 0;
    if (!WriteFile(m_h, buf, n, &written, nullptr) || written != static_cast<DWORD>(n)) {
        m_lastError = L"Ошибка передачи данных (код " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    m_tx.fetch_add(1);
    return true;
}

void SerialPort::rxLoop()
{
    Utf8Decoder decoder;
    unsigned char buffer[256];

    while (m_running.load()) {
        if (m_h != INVALID_HANDLE_VALUE) {
            DWORD bytesRead = 0;
            if (ReadFile(m_h, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
                for (DWORD i = 0; i < bytesRead; ++i) {
                    wchar_t ch = 0;
                    if (decoder.push(buffer[i], ch) && m_cb)
                        m_cb(ch);
                }
            }
        }
        Sleep(1);
    }
}
