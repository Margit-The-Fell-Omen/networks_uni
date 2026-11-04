#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace channel
{
    // Вариант 1: занятость канала 10 %, коллизия 40 %,
    // коллизия рассматривается в отношении отдельного байта,
    // дополнение к алгоритму - jam-сигнал.
    constexpr int BUSY_PERCENT      = 10;
    constexpr int COLLISION_PERCENT = 40;
    constexpr int MAX_ATTEMPTS      = 16;
    constexpr int BUSY_WAIT_MS      = 10;
    constexpr int SLOT_TIME_MS      = 50;
    constexpr int JAM_BYTES         = 4;
    constexpr uint8_t JAM_VALUE     = 0xFF;
    constexpr int CHUNK_BYTES       = 8;

    struct Config
    {
        bool enabled = true;
        int busyPercent = BUSY_PERCENT;
        int collisionPercent = COLLISION_PERCENT;
        int maxAttempts = MAX_ATTEMPTS;
        int busyWaitMs = BUSY_WAIT_MS;
        int slotTimeMs = SLOT_TIME_MS;
        int jamBytes = JAM_BYTES;
        uint8_t jamValue = JAM_VALUE;
        int chunkBytes = CHUNK_BYTES;
    };

    enum class State
    {
        Idle,
        Listening,
        Busy,
        Transmitting,
        Collision,
        Jam,
        Backoff,
        Done,
        Error,
        Cancelled
    };

    const wchar_t *stateName(State state);

    struct Result
    {
        bool success = false;
        bool cancelled = false;
        bool mediumError = false;
        int collisions = 0;
        int attempts = 0;
        int busyWaits = 0;
        int backoffMs = 0;
    };

    // Пошаговая реализация CSMA/CD. Один tick() выполняет один шаг:
    // прослушивание, передачу порции байт, jam-сигнал или задержку.
    // Отдельный поток передаче не нужен, поэтому интерфейс остаётся отзывчивым.
    class Session
    {
    public:
        using SendFn = std::function<bool(const uint8_t *data, size_t size)>;
        using StateFn = std::function<void(State state)>;
        using CollisionFn = std::function<void(int byteIndex, int attempt)>;
        using BackoffFn = std::function<void(int delayMs)>;
        using ProgressFn = std::function<void(int sent, int total)>;

        Session();

        void setSend(SendFn fn) { m_send = std::move(fn); }
        void setOnState(StateFn fn) { m_onState = std::move(fn); }
        void setOnCollision(CollisionFn fn) { m_onCollision = std::move(fn); }
        void setOnBackoff(BackoffFn fn) { m_onBackoff = std::move(fn); }
        void setOnProgress(ProgressFn fn) { m_onProgress = std::move(fn); }

        void start(std::vector<uint8_t> frame, const Config &config);
        bool tick();
        void cancel();

        bool running() const { return m_running; }
        State state() const { return m_state; }
        const Result &result() const { return m_result; }

    private:
        enum class Phase { Idle, Listen, WaitBusy, Send, WaitBackoff, Finished };

        void setState(State state);
        bool roll(int percent);
        int randomInt(int min, int max);
        void waitFor(int ms);
        void finish(State state);

        std::vector<uint8_t> m_frame;
        Config m_config;
        Result m_result;
        Phase m_phase = Phase::Idle;
        State m_state = State::Idle;
        bool m_running = false;

        int m_sent = 0;
        int m_collisionAt = -1;
        bool m_attemptCollision = false;
        std::chrono::steady_clock::time_point m_wake;

        std::mt19937 m_rng;

        SendFn m_send;
        StateFn m_onState;
        CollisionFn m_onCollision;
        BackoffFn m_onBackoff;
        ProgressFn m_onProgress;
    };
}
