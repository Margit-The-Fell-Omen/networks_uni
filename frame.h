//
// Created by Михаил on 29.09.2026.
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace frame
{
    constexpr size_t  FLAG_LEN = 8;
    constexpr uint8_t FLAG[FLAG_LEN] = {'4', '5', '0', '5', '0', '1', '-', '1'};

    // Параметры кодирования (циклический код, вариант 1).
    // Расчёт длины поля FCS:
    //   k = MAX_DATA_LEN * 8 = 77 * 8 = 616 бит - максимальная длина поля данных;
    //   код должен исправлять одиночные ошибки, т.е. все n = k + r позиций ошибки
    //   должны давать различные ненулевые синдромы: 2^r >= n + 1 = k + r + 1;
    //   r = 9:  2^9  = 512  <  616 + 9  + 1 = 626  - не хватает;
    //   r = 10: 2^10 = 1024 >= 616 + 10 + 1 = 627  - хватает, r_min = 10 бит;
    //   округляем до байтов: ceil(10 / 8) = 2 байта = 16 бит = степень порождающего многочлена.
    //   g(x) = x^16 + x^12 + x^5 + 1 (0x1021) = (x + 1) * p(x), где период p(x) = 32767 >= n = 632,
    //   поэтому одиночная ошибка в любой из 632 позиций даёт уникальный синдром.
    // Один байт FCS был в исходном кадре, второй взят из резерва (RESERVED_LEN: 10 -> 9),
    // длина кадра не изменилась.
    constexpr size_t FCS_LEN       = 2;
    constexpr size_t RESERVED_LEN  = 9;
    constexpr size_t SERVICE_LEN   = 2 + FCS_LEN;   // адрес назначения + адрес источника + FCS
    constexpr size_t MAX_FRAME_LEN = 100;
    constexpr size_t MAX_DATA_LEN  = 77;

    constexpr size_t MAX_BODY_BITS    = (SERVICE_LEN + MAX_DATA_LEN + RESERVED_LEN) * 8;
    constexpr size_t MAX_STUFFED_BITS = MAX_BODY_BITS + MAX_BODY_BITS / 63;
    constexpr size_t MAX_STUFFED_LEN  = FLAG_LEN + (MAX_STUFFED_BITS + 7) / 8;

    // | Флаг (8) | Адрес назначения (1) | Адрес источника (1) | Данные (1..77) | FCS (2) | Резерв (9) |
    struct Frame
    {
        uint8_t destination = 0;   // пока 0. Будущее: адрес станции-получателя (игнорировать чужие кадры)
        uint8_t source = 0;        // пока 0. Будущее: адрес отправителя (для ответа / подтверждения)
        std::vector<uint8_t> data; // полезная нагрузка — UTF-8 байты сообщения
        uint8_t fcs[FCS_LEN] = {}; // циклический код только от поля данных
        uint8_t reserved[RESERVED_LEN] = {};
    };

    // Вероятность искажения кадра при включённой эмуляции ошибок, %.
    // Вариант 1 - 10 %, вариант 2 - 20 %, вариант 3 - 30 %, вариант 4 - 40 %.
    constexpr int ERROR_PERCENT = 10;

    // Эмуляция ошибки: с вероятностью percent % инвертирует один случайный бит
    // одного случайного байта в поле данных.
    struct ErrorInfo
    {
        bool applied = false;
        size_t byteIndex = 0;   // номер байта в поле данных
        int bitIndex = 0;       // номер бита в байте, 0 - старший (как при выводе)
    };
    ErrorInfo injectError(Frame &f, int percent);

    // Тело кадра (всё, кроме флага) в байтах
    std::vector<uint8_t> serializeBody(const Frame &f);
    bool parseBody(const std::vector<uint8_t> &body, Frame &f);

    struct StuffResult
    {
        std::vector<uint8_t> bytes;    // готово к отправке: флаг + стаффленное тело + добивка до байта
        std::vector<bool> bits;        // стаффленное тело побитно (без добивки)
        std::vector<bool> inserted;    // true — бит вставлен стаффингом
    };
    StuffResult stuff(const std::vector<uint8_t> &body);
    // n байт после флага (до следующего флага) -> исходное тело
    std::vector<uint8_t> destuff(const uint8_t *p, size_t n);

    // Для окна состояния
    std::wstring fieldNames();
    struct FrameView
    {
        std::wstring before;             // значения до стаффинга
        std::wstring after;              // после стаффинга
        std::vector<bool> afterUnderline;// маска подчёркивания для каждого символа after
        bool hasErrorRow = false;
        std::wstring afterError;         // после эмуляции ошибки (в том виде, как ушло в порт)
        std::vector<bool> errorUnderline;// подчёркнут искажённый бит
    };
    FrameView makeView(const Frame &f, const StuffResult &s);
    // f, s - кадр без искажения; sent, sentStuffed - кадр, фактически отправленный в порт
    FrameView makeView(const Frame &f, const StuffResult &s,
                       const Frame &sent, const StuffResult &sentStuffed, const ErrorInfo &err);

    // Сборщик кадров на приёмной стороне
    class Receiver
    {
    public:
        // Возвращает принятые кадры
        std::vector<Frame> feed(const uint8_t *p, size_t n);
        std::vector<Frame> flush();   //конец последнего кадра
    private:
        std::vector<uint8_t> m_buf;
        void process(std::vector<Frame> &out, bool isFinal);
    };
}

namespace crc
{
    uint16_t calculateFCS(const std::vector<uint8_t> &data);
    // Записывает FCS в кадр (только по полю данных)
    void setFCS(frame::Frame &f);
    // true - кадр цел или ошибка исправлена (данные исправлены на месте)
    bool IsCorrect(frame::Frame &f);
}
