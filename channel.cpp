#include "channel.h"

#include <algorithm>

namespace channel
{
    const wchar_t *stateName(State state)
    {
        switch (state)
        {
        case State::Idle:         return L"ожидание";
        case State::Listening:    return L"прослушивание канала";
        case State::Busy:         return L"канал занят";
        case State::Transmitting: return L"передача кадра";
        case State::Collision:    return L"коллизия";
        case State::Jam:          return L"jam-сигнал";
        case State::Backoff:      return L"случайная задержка";
        case State::Done:         return L"кадр передан";
        case State::Error:        return L"ошибка передачи";
        case State::Cancelled:    return L"отменено";
        }
        return L"";
    }

    Session::Session()
        : m_rng(static_cast<unsigned>(std::random_device{}()) ^
                static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()))
    {
    }

    bool Session::roll(int percent)
    {
        if (percent <= 0)
            return false;
        if (percent >= 100)
            return true;
        return std::uniform_int_distribution<int>(0, 99)(m_rng) < percent;
    }

    int Session::randomInt(int min, int max)
    {
        if (max <= min)
            return min;
        return std::uniform_int_distribution<int>(min, max)(m_rng);
    }

    void Session::setState(State state)
    {
        m_state = state;
        if (m_onState)
            m_onState(state);
    }

    void Session::waitFor(int ms)
    {
        m_wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    }

    void Session::finish(State state)
    {
        m_running = false;
        m_phase = Phase::Finished;
        setState(state);
    }

    void Session::start(std::vector<uint8_t> frame, const Config &config)
    {
        m_frame = std::move(frame);
        m_config = config;
        m_config.chunkBytes = std::max(1, m_config.chunkBytes);
        m_result = Result{};
        m_sent = 0;
        m_collisionAt = -1;
        m_attemptCollision = false;
        m_running = true;

        if (m_frame.empty())
        {
            m_result.success = true;
            m_result.attempts = 1;
            finish(State::Done);
            return;
        }

        // Без эмуляции моноканала кадр просто уходит в порт.
        if (!m_config.enabled)
        {
            m_result.attempts = 1;
            m_phase = Phase::Send;
            setState(State::Transmitting);
            return;
        }

        m_phase = Phase::Listen;
        setState(State::Listening);
    }

    bool Session::tick()
    {
        if (!m_running)
            return false;

        const auto now = std::chrono::steady_clock::now();

        switch (m_phase)
        {
        case Phase::Listen:
            if (roll(m_config.busyPercent))
            {
                ++m_result.busyWaits;
                setState(State::Busy);
                waitFor(m_config.busyWaitMs);
                m_phase = Phase::WaitBusy;
                return true;
            }

            ++m_result.attempts;
            m_attemptCollision = roll(m_config.collisionPercent);
            m_collisionAt = m_attemptCollision
                ? randomInt(0, static_cast<int>(m_frame.size()) - 1)
                : -1;
            m_sent = 0;
            m_phase = Phase::Send;
            setState(State::Transmitting);
            return true;

        case Phase::WaitBusy:
            if (now < m_wake)
                return true;
            m_phase = Phase::Listen;
            setState(State::Listening);
            return true;

        case Phase::Send:
        {
            // При коллизии "в отношении байта" передача обрывается
            // на выбранном байте; номер байта передаётся в интерфейс.
            int limit = static_cast<int>(m_frame.size());
            if (m_attemptCollision && m_collisionAt >= 0)
                limit = m_collisionAt + 1;

            const int chunk = std::min(m_config.chunkBytes, limit - m_sent);
            if (chunk <= 0)
            {
                m_result.success = true;
                finish(State::Done);
                return false;
            }

            if (!m_send || !m_send(m_frame.data() + m_sent, static_cast<size_t>(chunk)))
            {
                m_result.mediumError = true;
                finish(State::Error);
                return false;
            }

            m_sent += chunk;
            if (m_onProgress)
                m_onProgress(m_sent, static_cast<int>(m_frame.size()));

            if (m_attemptCollision && m_sent > m_collisionAt)
            {
                ++m_result.collisions;
                setState(State::Collision);
                if (m_onCollision)
                    m_onCollision(m_collisionAt, m_result.attempts);

                // Jam-сигнал: им передатчик сообщает о коллизии.
                setState(State::Jam);
                if (m_config.jamBytes > 0)
                {
                    std::vector<uint8_t> jam(static_cast<size_t>(m_config.jamBytes), m_config.jamValue);
                    if (!m_send || !m_send(jam.data(), jam.size()))
                    {
                        m_result.mediumError = true;
                        finish(State::Error);
                        return false;
                    }
                }

                if (m_result.collisions >= m_config.maxAttempts)
                {
                    finish(State::Error);
                    return false;
                }

                // Стандартная двоичная экспоненциальная задержка:
                // после n-й коллизии k = random(0 .. 2^n - 1), пауза k * slot.
                const int n = std::min(m_result.collisions, 10);
                const int maxK = (1 << n) - 1;
                const int delay = randomInt(0, maxK) * m_config.slotTimeMs;
                m_result.backoffMs += delay;

                waitFor(delay);
                m_phase = Phase::WaitBackoff;
                setState(State::Backoff);
                if (m_onBackoff)
                    m_onBackoff(delay);
                return true;
            }

            if (m_sent >= static_cast<int>(m_frame.size()))
            {
                m_result.success = true;
                finish(State::Done);
                return false;
            }
            return true;
        }

        case Phase::WaitBackoff:
            if (now < m_wake)
                return true;
            m_phase = Phase::Listen;
            setState(State::Listening);
            return true;

        case Phase::Finished:
            return false;
        }

        return false;
    }

    void Session::cancel()
    {
        if (!m_running)
            return;
        m_result.cancelled = true;
        finish(State::Cancelled);
    }
}
