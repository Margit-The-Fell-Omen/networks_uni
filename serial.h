#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>

class SerialPort
{
public:
    using RxCallback = std::function<void(wchar_t)>;

    ~SerialPort() { close(); }

    std::vector<std::wstring> enumeratePorts();

    bool open(const std::wstring &port, DWORD baud);
    void close();
    bool send(wchar_t ch);

    bool isOpen() const { return m_h != INVALID_HANDLE_VALUE; }
    long long transmitted() const { return m_tx.load(); }
    const std::wstring &lastError() const { return m_lastError; }

    void setRxCallback(RxCallback cb) { m_cb = std::move(cb); }

private:
    void rxLoop();

    HANDLE m_h = INVALID_HANDLE_VALUE;
    std::thread m_rx;
    std::atomic<bool> m_running{false};
    std::atomic<long long> m_tx{0};
    std::wstring m_lastError;
    RxCallback m_cb;
};
