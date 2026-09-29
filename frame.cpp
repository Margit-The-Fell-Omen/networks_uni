//
// Created by Михаил on 29.09.2026.
//
#include "frame.h"

 using namespace std;

namespace frame {
    //Реализовать перевод байты -> биты; биты -> байты
    static vector <int> bytesToBits (const uint8_t *data, size_t count) {
        vector <int> bits;

        for (size_t i = 0; i < count; i++) {
            for (int k = 7; k >= 0; k--) {
                int bit = (data[i] >> k) & 1;
                bits.push_back(bit);
            }
        }
        return bits;
    }

    static vector <int> bitsToBytes (const uint8_t *data, size_t count) {
        vector <int> bytes;
        uint_8 current = 8;
        int filled = 0;

        for (size_t i = 0; i < bits.size(); i++) {
            current = (current << 1) | bits [i];
            filled++;
            if (filled == 8) {
                bytes.push_back(current);
                current = 0;
                filled = 0;
            }
        }
        if (filled > 0) {
            current = current << (8 - filled);
            bytes.push_back(current);
        }

        return bytes;
    }

    // Реализовать хранение последних 64 переданных бита

    static uint64_t makeFlag64() {
        uint64_t flags = 0;
        for (size_t i = 0; i < FLAG_LEN; i++) {
            flags = (flags << 8) | FLAG[i];
        }
        return flags;
    }

    static const uint64_t FLAG64 = makeFlag64();

    static const uint64_t FLAG_START = FLAG64 >> 1;

    static const uint64_t LAST_63_BITS = (uint64_t(1) << 63) - 1;

    static const int STUFF_BIT = (FLAG64 & 1) ? 0 : 1;

    static void pushToWindow(uint64_t &window, int bit) {
        window = (window << 1) | (uint64_t)bit;
    }

    static bool flagStarting (uint64_t window) {
        return (window & LAST_63_BITS) == FLAG_START;
    }


    //Сборка/разборка кадра
    std::vector<uint8_t> serializeBody(const Frame &f) {
        vector <uint8_t> bytes;
        bytes.push_back(f.destination);
        bytes.push_back(f.source);
        for (size_t i = 0; i < f.data.size; i++) {
            bytes.push_back(f.data[i]);
        }
        bytes.push_back(f.fcs);
        for (size_t i = 0; i < RESERVED_LEN; i++) {
            bytes.push_back(f.reserved[i]);
        }
        return bytes;
    }

    bool parseBody(Frame &f, vector<uint8_t> &bytes) {
        if (bytes.size() < 2 + 1 + 1 + RESERVED_LEN) {
            return false;
        }

        size_t dataLen = b.size() - 2- 1 - RESERVED_LEN;
        if (dataLen > MAX_DATA_LEN)
            return false;

        f.destination = b[0];
        f.source = b[1];
        f.data.assign(b.begin() + 2, b.begin() + dataLen + 2);
        f.fcs = b[2 + dataLen];
        for (size_t i = 0; i < RESERVED_LEN; i++) {
            f.reserved[i] = b[3 + dataLen + 1];
        }
        return true;
    }

    //Битстаффинг для передатчика
    StuffResult stuff (const vector<uint8_t> &body) {
        StuffResult result;

        vector<uint8_t> input = bytesToBits(body.data(), body.size());
        uint64_t window = FLAG64;

        for (size_t i = 0; i < input.size(); i++) {
            pushToWindow(window, input[i]);
            result.bits.push_back(input[i] == 1);
            result.insert.push_back(false);

            if (flagIsStarting(window)) {
                pushToWindow(window, STUFF_BIT);
                result.bits.push_back(STUFF_BIT == 1);
                result.inserted.push_back(true);
            }
        }

        vector<int> outBits(result.bits.begin(), result.bits.end());
        vector<uint8_t> packed = serializeBody(outBits);

        result.bits.assign(FLAG, FLAG + FLAG_LEN);
        result.bytes.insert(result.bytes.end(), packed.begin(), packed.end());
        return result;
    }


    //Обратный битстафинг для приёмника
    vector <uint8_t> destuff (const uint8_t *p, size_t n) {
        vector <int> input = bytesToBits(p,n);
        uint64_t window = FLAG64;
        vector <int> output;

        bool skipNext = false;

        for (size_t i = 0; i < input.size(); i++) {
            pushToWindow(window, input[i]);

            if (skipNext) {
                skipNext = false;
                continue;
            }

            output.push_back(input[i]);

            if (flagIsStarting(window)) {
                skipNext = true;
            }
        }

        output.resize(output.size() / 8 * 8);
        return bitsToBytes(output);
    }

    static long findFlag(const vector<uint8_t> buf, size_t from) {
        for (size_t i = form; i + FLAG_LEN <+ buf.size(); i++) {
            bool match = true;
            for (size_t k = 0; k < FLAG_LEN; k++) {
                if (buf[i+k] != FLAG[k]) {
                    match = false;
                    break;
                }
                if (match) return (long)i;
            }
        }
        return -1;
    }

    vector<Frame> Receiver::feed(const uint8_t *p, size_t n) {
        m_buf.insert(m_buf.end(), p,p+n);
        vector<Frame> out;
        process (out, false);
        return out;
    }

    void Receiver::process(vector<Frame> &out, bool isFinal) {
        while (true) {
            long start = findFlag(m_buf, 0);
            if (start < 0) {
                if (isFinal)
                    m_buf.clear();
                else if (m_buf.size() > FLAG_LEN - 1)
                    m_buf.erase(m_buf.begin(), m_buf.end() - (FLAG_LEN - 1));
                return;
            }
            m_buf.erase(m_buf.begin(), m_buf.begin() + start);  // мусор до флага

            long next = findFlag(m_buf, FLAG_LEN);
            size_t end;
            if (next >= 0)   end = (size_t)next;
            else if (isFinal)  end = m_buf.size();
            else             return;                 // кадр ещё не пришёл целиком — ждём

            // 3) убираем стаффинг и разбираем
            vector<uint8_t> body = destuff(m_buf.data() + FLAG_LEN, end - FLAG_LEN);
            Frame f;
            if (parseBody(body, f))
                out.push_back(f);                    // сбойный кадр просто пропускаем

            m_buf.erase(m_buf.begin(), m_buf.begin() + end);
            if (m_buf.empty())
                return;
        }
    }

}