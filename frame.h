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

    constexpr size_t RESERVED_LEN  = 10;
    constexpr size_t SERVICE_LEN   = 3;     // адрес назначения + адрес источника + FCS
    constexpr size_t MAX_FRAME_LEN = 100;

    constexpr size_t MAX_DATA_LEN  = 77;

    constexpr size_t MAX_BODY_BITS    = (SERVICE_LEN + MAX_DATA_LEN + RESERVED_LEN) * 8;
    constexpr size_t MAX_STUFFED_BITS = MAX_BODY_BITS + MAX_BODY_BITS / 63;
    constexpr size_t MAX_STUFFED_LEN  = FLAG_LEN + (MAX_STUFFED_BITS + 7) / 8;

    // | Флаг (8) | Адрес назначения (1) | Адрес источника (1) | Данные (1..77) | FCS (1) | Резерв (10) |
    struct Frame
    {
        uint8_t destination = 0;   // пока 0. Будущее: адрес станции-получателя (игнорировать чужие кадры)
        uint8_t source = 0;        // пока 0. Будущее: адрес отправителя (для ответа / подтверждения)
        std::vector<uint8_t> data; // полезная нагрузка — UTF-8 байты сообщения
        uint8_t fcs = 0;           // пока 0. Будущее: контрольная сумма/CRC-8 для обнаружения сбойных кадров
        uint8_t reserved[RESERVED_LEN] = {};
    };

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
    };
    FrameView makeView(const Frame &f, const StuffResult &s);

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
