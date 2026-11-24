#pragma once

#include <algorithm>
#include <atomic>
#include <functional>

namespace Core {

    /**
     * Lightweight progress reporting sink supporting either lock-free atomic updates (GUI)
     * or direct callbacks (CLI / batch jobs).
     */
    class ProgressReporter {
    public:
        ProgressReporter() = default;

        static ProgressReporter FromAtomic(std::atomic<float>& target) noexcept {
            ProgressReporter r;
            r.m_atomic = &target;
            return r;
        }

        static ProgressReporter FromCallback(std::function<void(float)> callback) {
            ProgressReporter r;
            r.m_callback = std::move(callback);
            return r;
        }

        static ProgressReporter Null() noexcept {
            return {};
        }

        void Report(float value) const {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            if (m_atomic) {
                m_atomic->store(clamped, std::memory_order_relaxed);
            }
            if (m_callback) {
                m_callback(clamped);
            }
        }

        void Complete() const {
            Report(1.0f);
        }

    private:
        std::atomic<float>* m_atomic = nullptr;
        std::function<void(float)> m_callback;
    };

} // namespace Core
