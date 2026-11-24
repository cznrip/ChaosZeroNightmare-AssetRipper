#pragma once

#include "core/Logger.hpp"
#include "core/ProgressReporter.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <optional>
#include <string>

namespace Services {

    enum class TaskKind {
        None,
        Scan,
        Extract
    };

    struct TaskCompletion {
        TaskKind kind = TaskKind::None;
        bool succeeded = true;
        std::string error;
    };

    /**
     * Manages a single background archive operation (Scan or Extract) with safe
     * exception propagation, progress reporting, and non-blocking completion polling.
     */
    class AsyncTaskRunner {
    public:
        ~AsyncTaskRunner() {
            Wait();
        }

        [[nodiscard]] bool IsRunning() const noexcept {
            return m_running.load(std::memory_order_relaxed);
        }

        [[nodiscard]] float Progress() const noexcept {
            return m_progress.load(std::memory_order_relaxed);
        }

        [[nodiscard]] TaskKind Kind() const noexcept { return m_kind; }
        [[nodiscard]] const std::string& Status() const noexcept { return m_status; }
        void SetStatus(std::string status) { m_status = std::move(status); }

        bool Start(const TaskKind kind, std::string initial_status, std::function<void(Core::ProgressReporter)> work) {
            if (IsRunning()) return false;

            Wait();
            m_kind = kind;
            m_status = std::move(initial_status);
            m_progress.store(0.0f, std::memory_order_relaxed);
            m_running.store(true, std::memory_order_relaxed);

            try {
                auto reporter = Core::ProgressReporter::FromAtomic(m_progress);
                m_future = std::async(std::launch::async, [fn = std::move(work), reporter]() mutable {
                    fn(reporter);
                });
                return true;
            } catch (const std::exception& e) {
                m_running.store(false, std::memory_order_relaxed);
                m_kind = TaskKind::None;
                m_status = std::string("Error starting task: ") + e.what();
                LogError(m_status);
                return false;
            }
        }

        [[nodiscard]] std::optional<TaskCompletion> PollCompletion() {
            if (!IsRunning() || !m_future.valid()) return std::nullopt;
            if (m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return std::nullopt;
            }

            TaskCompletion result;
            result.kind = m_kind;
            m_running.store(false, std::memory_order_relaxed);
            m_kind = TaskKind::None;
            m_progress.store(1.0f, std::memory_order_relaxed);

            try {
                m_future.get();
                result.succeeded = true;
            } catch (const std::exception& e) {
                result.succeeded = false;
                result.error = e.what();
                m_status = "Error: " + result.error;
                LogError("Background task failed: " + result.error);
            } catch (...) {
                result.succeeded = false;
                result.error = "Unknown error occurred";
                m_status = result.error;
                LogError("Background task failed with unknown exception");
            }
            return result;
        }

        void Wait() {
            if (m_future.valid()) {
                try {
                    m_future.get();
                } catch (...) {}
            }
            m_running.store(false, std::memory_order_relaxed);
        }

    private:
        std::future<void> m_future;
        std::atomic<float> m_progress{0.0f};
        std::atomic<bool> m_running{false};
        TaskKind m_kind = TaskKind::None;
        std::string m_status = "Select a data.pack or manifest.ssra file to begin.";
    };

} // namespace Services
