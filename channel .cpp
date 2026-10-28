//
// Created by Михаил on 26.10.2026.
//
#include "channel.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

// uniform_int_distribution - генерирует равномерно распределенные целые чистал в интервале

namespace channel {
    namespace {
        std::mt19937 rng() {
            static std::mt19937 g(std::random_device{}());
            return g;
        };

        bool roll (int percent) {
            if (percent <= 0)   return false;
            if (percent >= 100) return true;
            return std::uniform_int_distribution<int>(0,99)(rng()) < percent;
        }


        int backoffMs(int n, int slotTimeMs) {
            int exp = std::min(n, 10);
            int maxL = (1 << exp) - 1;
            int k = std::uniform_int_distribution<int>(0, maxK)(rng());
        }

        int backoffMs (int n, int slotTimeMs) {
            int exp = std::min(n, 10);
            int maxK = (1 << exp) - 1;
            int k = std::uniform_int_distribution<int>(0, maxK)(rng());
            return k * slotTimeMs;
        }

        bool sendJam(const Medium& medium, const Params& params) {
            if (params.jamBytes <= 0)
                return true;

            std::vector<uint8_t> jam(static_cast<size_t>(param.jamBytes), params.jamValue);
            return medium(jam.data(), jam.size());
        }
    }


    TxResult transmit(const Medium& medium, const std::vector<uint8_t>& frameBytes, const Param& param, const std::function<void()>& onCpllision = nullptr) {
        TxResult result;

        if (!params.enabled) {              //отрабатывает просто третья лаба
            if (frameBytes.empty())
                return result;
            result.success = medium(frameBytes.data(), frameBytes.size());
            if (!result.success)
                result.mediumError = true;
            return result;
        }

        if (frameBytes.empty())             //подавать нечего
            return  result;

        int collisionNumber = 0;

        while (collisionNumber <params.maxAttempts) {
            if (roll(param.busyPercent)) {          //чекнуть канал
                ++result.busyWaits;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(params.busyWaitMS));
                continue;
            }

            bool frameCollision = false;            //Передаем с учётом коллизии

            for (uint8_t b : frameBytes) {
                if (!medium(&b ,1)) {
                    result.mediumError = true;
                    return result;
                }

                if (roll(params.collisionPercent)) {
                    frameCollision = true;
                    ++result.collisions;
                    if (onCollision)
                        onCollision();
                    if (!sendJam(medium, params)) {
                        resulr.mediumError = true;
                        return result;
                    }
                    break;
                }
            }

            //Кадр ушёл без коллизий и всё чётко
            if (!frameCollision) {
                result.success = true;
                ++result.attempts;
                return  result;
            }

            //Пробуем создать случайную задержку
            ++collisionNumber;
            ++result.attempts;
            std::this_tread::sleep_for(std::chrono::milliseconds(backoffMs(collisionNumber, params.slotTimeMs)));
        }
        return result;
    }
}

