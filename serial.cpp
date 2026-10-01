#include "serial.h"

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

bool SerialPort::sendBytes(const uint8_t *data, size_t n)
{
    if (m_h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    if (!WriteFile(m_h, data, static_cast<DWORD>(n), &written, nullptr) || written != static_cast<DWORD>(n)) {
        m_lastError = L"Ошибка передачи данных (код " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    m_tx.fetch_add(written);
    return true;
}

void SerialPort::rxLoop()
{
    unsigned char buffer[256];

    while (m_running.load()) {
        if (m_h != INVALID_HANDLE_VALUE) {
            DWORD bytesRead = 0;
            if (ReadFile(m_h, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
                for (DWORD i = 0; i < bytesRead; ++i) {
                    if (m_cb)
                        m_cb(buffer[i]);
                }
            }
        }
        Sleep(1);
    }
}
