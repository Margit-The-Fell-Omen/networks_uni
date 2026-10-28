#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace channel {
    constexpr int BUSY_PERCENT = 10;
    constexpr int COLLISION_PERCENT = 40;

    using Medium = std::function<bool(const uint8_t* data, size_t n)>;

    struct Param {
        bool enable                  = true;
        int busyPercent              = BUSY_PERCENT;
        int collisionPercent         = COLLISION_PERCENT;
        int maxAttempts              = 16;
        int jamBytes                 = 4;
        uint8_t jamValue             = 0xFF;
        int slotTimeMs               = 50;
        int busyWaitMs               = 10;
    };

    struct TxResult {
        bool success = false;
        int collisions = 0;
        int attempts = 0;
        int bussyWaits = 0;
        bool mediumError = false;
    };

    TxResult transmit(const Medium& medium,
                const std::vector<uint8_t>& frameBytes,
                const Param& param,
                const std::function<void()>& onCpllision = nullptr);
}

