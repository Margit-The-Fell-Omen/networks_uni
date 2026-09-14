#include <iostream>
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <setupapi.h>

using namespace std;

struct ComPortInfo {
    wstring friendlyName;
    DCB dcb{};
    bool dcbValid = false;
}

class ComPortEnumerator {
public:
    static std::vector<ComPortInfo> Enumerate();
 
private:
    static bool GetDcb(const std::wstring& portName, DCB& dcbOut);
};


static vector <ComPortInfo> ComPortEnumerator::Enumerate() {
    vector<ComPortInfo> result;
    HKEY hKey;
    LONG openResult = RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
        0, KEY_READ, &hKey);
    if (openResult != ERROR_SUCCESS) {
        return result;
    }
    wchar_t valueName[256];
    BYTE valueData[256];
 
    for (DWORD index = 0; ; ++index) {
        DWORD valueNameSize = ARRAYSIZE(valueName);
        DWORD valueDataSize = sizeof(valueData);
        DWORD type = 0;
 
        LONG enumResult = RegEnumValueW(
            hKey, index,
            valueName, &valueNameSize,
            nullptr, &type,
            valueData, &valueDataSize);
 
        if (enumResult == ERROR_NO_MORE_ITEMS) break;
        if (enumResult != ERROR_SUCCESS || type != REG_SZ) continue;
 
        ComPortInfo info;
        info.portName = reinterpret_cast<wchar_t*>(valueData);
        info.dcbValid = GetDcb(info.portName, info.dcb);
 
        result.push_back(std::move(info));
    }
 
    RegCloseKey(hKey);
    return result;
}


bool ComPortEnumerator::GetDcb(const wstring& portName, DCB& dcbOut) {
    if (portName.empty()) return false;
 
    wstring path = L"\\\\.\\" + portName;
 
    HANDLE hComm = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
 
    if (hComm == INVALID_HANDLE_VALUE) return false;
 
    ZeroMemory(&dcbOut, sizeof(DCB));
    dcbOut.DCBlength = sizeof(DCB);
    bool ok = GetCommState(hComm, &dcbOut) != 0;
 
    CloseHandle(hComm);
    return ok;
}


























class SerialTransiver {
private:
    HANDLE hSerial = INVALID_HANDLE_VALUE;
    thread rxThread;
    atomic<bool> Running {false};

    void readLoop() {
        char buffer[256];
        DWORD bytesRead;

        while (Running) {
            if (ReadFile(hSerial,buffer, sizeof(buffer) - 1, &bytesRead,NULL )) {
                if (bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    cout << "\n [ получено] :" << buffer << "\n" << flush;
                }
            }
            this_thread::sleep_for((chrono::milliseconds(10)));
        }
    }

public:
    ~SerialTransiver() {
        disconnect();
    }

    bool connect (const string &portname, DWORD baudrate = CBR_9600) {
        hSerial = CreateFile (portname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hSerial == INVALID_HANDLE_VALUE) {
            cerr << "Порт не открылся!!!!" << portname << "(код: " << GetLastError << ")\n";
            return false;
        }

        DCB dcbSerialParam = { 0 };
        dcbSerialParam.DCBlength = sizeof(dcbSerialParam);
        if (!GetCommState(hSerial, &dcbSerialParam)) {
            cerr << "Ошибка получения параметров DCB. \n";
            return false;
        }

        //Настройка параметров порта (по умолчанию)
        dcbSerialParam.BaudRate = baudrate;
        dcbSerialParam.ByteSize = 8;
        dcbSerialParam.StopBits = ONESTOPBIT;
        dcbSerialParam.Parity = NOPARITY;
        dcbSerialParam.fOutxCtsFlow = FALSE;
        dcbSerialParam.fOutxDsrFlow = FALSE;
        dcbSerialParam.fDtrControl = DTR_CONTROL_DISABLE;
        dcbSerialParam.fRtsControl = RTS_CONTROL_DISABLE;

        if (!SetCommState(hSerial, &dcbSerialParam)) {
            cerr << "Ошибка установки параметров DCB. \n";
            return false;
        }

        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 100;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutMultiplier = 10;

        if (!SetCommTimeouts(hSerial, &timeouts)) {
            cerr << "ТАЙМАУТЫ СЛОМАНЫ!!! \n";
            return false;
        }

        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

        Running = true;
        rxThread = thread(&SerialTransiver::readLoop, this);
        return true;
    }

    bool send (const string& message) {
        if (hSerial == INVALID_HANDLE_VALUE) {
            return false;
            DWORD bytesWritten;
            bool result = WriteFile(hSerial, message.c_str(), message.length(), &bytesWritten, NULL);
            return result && (bytesWritten == message.length());
        }
    }

    void disconnect() {
        if (Running) {
            Running = false;
            if (rxThread.joinable()) {
                rxThread.join();

            }
            if (hSerial != INVALID_HANDLE_VALUE) {
                CloseHandle (hSerial);
                hSerial = INVALID_HANDLE_VALUE;
            }
        }
    }


};

// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.

int main() {


    return 0;
}
