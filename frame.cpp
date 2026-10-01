//
// Created by Михаил on 29.09.2026.
//
#include "frame.h"

 using namespace std;

namespace frame {
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

    static vector <uint8_t> bitsToBytes (const vector <int> &bits) {
        vector <uint8_t> bytes;
        uint8_t current = 0;
        int filled = 0;

        for (size_t i = 0; i < bits.size(); i++) {
            current = (current << 1) | (uint8_t)bits[i];
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

    static bool flagStarting(uint64_t window) {
        return (window & LAST_63_BITS) == FLAG_START;
    }

    //Сборка/разборка кадра
    std::vector<uint8_t> serializeBody(const Frame &f) {
        vector <uint8_t> bytes;
        bytes.push_back(f.destination);
        bytes.push_back(f.source);
        bytes.insert(bytes.end(), f.data.begin(), f.data.end());
        bytes.push_back(f.fcs);
        for (size_t i = 0; i < RESERVED_LEN; i++) {
            bytes.push_back(f.reserved[i]);
        }
        return bytes;
    }

    bool parseBody(const vector<uint8_t> &body, Frame &f) {
        if (body.size() < 2 + 1 + 1 + RESERVED_LEN) {
            return false;
        }

        size_t dataLen = body.size() - 2 - 1 - RESERVED_LEN;
        if (dataLen > MAX_DATA_LEN)
            return false;

        f.destination = body[0];
        f.source = body[1];
        f.data.assign(body.begin() + 2, body.begin() + dataLen + 2);
        f.fcs = body[2 + dataLen];
        for (size_t i = 0; i < RESERVED_LEN; i++) {
            f.reserved[i] = body[3 + dataLen + i];
        }
        return true;
    }

    //Битстаффинг для передатчика
    StuffResult stuff (const vector<uint8_t> &body) {
        StuffResult result;

        vector <int> input = bytesToBits(body.data(), body.size());
        uint64_t window = FLAG64;

        for (size_t i = 0; i < input.size(); i++) {
            pushToWindow(window, input[i]);
            result.bits.push_back(input[i] == 1);
            result.inserted.push_back(false);

            if (flagStarting(window)) {
                pushToWindow(window, STUFF_BIT);
                result.bits.push_back(STUFF_BIT == 1);
                result.inserted.push_back(true);
            }
        }

        vector <int> outBits(result.bits.begin(), result.bits.end());
        vector <uint8_t> packed = bitsToBytes(outBits);

        result.bytes.assign(FLAG, FLAG + FLAG_LEN);
        result.bytes.insert(result.bytes.end(), packed.begin(), packed.end());
        return result;
    }

    //Обратный битстаффинг для приёмника
    vector <uint8_t> destuff (const uint8_t *p, size_t n) {
        vector <int> input = bytesToBits(p, n);
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

            if (flagStarting(window)) {
                skipNext = true;
            }
        }

        output.resize(output.size() / 8 * 8);
        return bitsToBytes(output);
    }

    static long findFlag(const vector<uint8_t> &buf, size_t from) {
        for (size_t i = from; i + FLAG_LEN <= buf.size(); i++) {
            bool match = true;
            for (size_t k = 0; k < FLAG_LEN; k++) {
                if (buf[i + k] != FLAG[k]) {
                    match = false;
                    break;
                }
            }
            if (match)
                return (long)i;
        }
        return -1;
    }

    vector<Frame> Receiver::feed(const uint8_t *p, size_t n) {
        m_buf.insert(m_buf.end(), p, p + n);
        vector<Frame> out;
        process(out, false);
        return out;
    }

    vector<Frame> Receiver::flush() {
        vector<Frame> out;
        process(out, true);
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

            vector<uint8_t> body = destuff(m_buf.data() + FLAG_LEN, end - FLAG_LEN);
            Frame f;
            if (parseBody(body, f))
                out.push_back(f);                    // сбойный кадр просто пропускаем

            m_buf.erase(m_buf.begin(), m_buf.begin() + end);
            if (m_buf.empty())
                return;
        }
    }

    static const size_t FIELD_COUNT = 5;

    static const wchar_t *FIELD_NAMES[FIELD_COUNT] = {
        L"Флаг", L"Адрес назначения", L"Адрес источника", L"Данные", L"FCS"
    };

    constexpr size_t COLUMN_WIDTH[FIELD_COUNT] = { FLAG_LEN * 9 - 1, 16, 15, 17, 8 };

    static std::wstring valueString(const uint8_t *p, size_t n) {
        std::wstring out;
        for (size_t i = 0; i < n; i++) {
            if (i)
                out += L',';
            for (int k = 7; k >= 0; k--)
                out += ((p[i] >> k) & 1) ? L'1' : L'0';
        }
        return out;
    }

    static void appendAligned(std::wstring &out, const std::wstring &value, size_t width) {
        for (size_t i = value.size(); i < width; i++)
            out += L' ';
        out += value;
    }

    std::wstring fieldNames() {
        std::wstring out;
        for (size_t k = 0; k < FIELD_COUNT; k++) {
            if (k)
                out += L' ';
            appendAligned(out, FIELD_NAMES[k], COLUMN_WIDTH[k]);
        }
        return out;
    }

    FrameView makeView(const Frame &f, const StuffResult &s) {
        FrameView view;

        appendAligned(view.before, valueString(FLAG, FLAG_LEN), COLUMN_WIDTH[0]);
        view.before += L' ';
        appendAligned(view.before, valueString(&f.destination, 1), COLUMN_WIDTH[1]);
        view.before += L' ';
        appendAligned(view.before, valueString(&f.source, 1), COLUMN_WIDTH[2]);
        view.before += L' ';
        appendAligned(view.before, valueString(f.data.data(), f.data.size()), COLUMN_WIDTH[3]);
        view.before += L' ';
        appendAligned(view.before, valueString(&f.fcs, 1), COLUMN_WIDTH[4]);

        size_t dataBits = f.data.size() * 8;
        const size_t starts[4] = { 0, 8, 16, 16 + dataBits };
        const size_t ends[4] = { 8, 16, 16 + dataBits, 24 + dataBits };

        std::vector<std::wstring> groups[4];
        std::vector<std::vector<bool> > masks[4];

        size_t orig = 0;
        for (size_t j = 0; j < s.bits.size(); j++) {
            bool inserted = j < s.inserted.size() && s.inserted[j];
            size_t owner = inserted && orig > 0 ? orig - 1 : orig;

            int field = -1;
            for (int k = 0; k < 4; k++) {
                if (owner >= starts[k] && owner < ends[k]) {
                    field = k;
                    break;
                }
            }

            if (field < 0) {
                if (!inserted)
                    orig++;
                continue;
            }

            size_t group = (owner - starts[field]) / 8;
            while (groups[field].size() <= group) {
                groups[field].push_back(std::wstring());
                masks[field].push_back(std::vector<bool>());
            }

            groups[field][group] += s.bits[j] ? L'1' : L'0';
            masks[field][group].push_back(inserted);

            if (!inserted)
                orig++;
        }

        std::wstring fields[4];
        std::vector<bool> fieldMasks[4];

        for (int k = 0; k < 4; k++) {
            for (size_t g = 0; g < groups[k].size(); g++) {
                if (g) {
                    fields[k] += L',';
                    fieldMasks[k].push_back(false);
                }
                fields[k] += groups[k][g];
                for (size_t b = 0; b < masks[k][g].size(); b++)
                    fieldMasks[k].push_back(static_cast<bool>(masks[k][g][b]));
            }
        }

        appendAligned(view.after, valueString(FLAG, FLAG_LEN), COLUMN_WIDTH[0]);
        view.afterUnderline.assign(view.after.size(), false);

        for (int k = 0; k < 4; k++) {
            view.after += L' ';
            view.afterUnderline.push_back(false);

            for (size_t i = fields[k].size(); i < COLUMN_WIDTH[k + 1]; i++) {
                view.after += L' ';
                view.afterUnderline.push_back(false);
            }

            view.after += fields[k];
            for (size_t b = 0; b < fieldMasks[k].size(); b++)
                view.afterUnderline.push_back(static_cast<bool>(fieldMasks[k][b]));
        }

        return view;
    }

}
