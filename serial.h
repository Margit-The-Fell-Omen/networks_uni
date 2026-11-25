#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>

class SerialPort
{
public:
    using RxCallback = std::function<void(uint8_t)>;

    ~SerialPort() { close(); }

    std::vector<std::wstring> enumeratePorts();

    bool open(const std::wstring &port, DWORD baud);
    void close();
    bool sendBytes(const uint8_t *data, size_t n);

    bool isOpen() const { return m_h != INVALID_HANDLE_VALUE; }
    long long transmitted() const { return m_tx.load(); }

    void setRxCallback(RxCallback cb) { m_cb = std::move(cb); }

private:
    void rxLoop();

    HANDLE m_h = INVALID_HANDLE_VALUE;
    std::thread m_rx;
    std::atomic<bool> m_running{false};
    std::atomic<long long> m_tx{0};
    RxCallback m_cb;
};
