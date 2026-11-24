#pragma once

#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace Logger {

    class FileSink {
    public:
        static FileSink& Instance() {
            static FileSink instance;
            return instance;
        }

        void Write(std::string_view level, std::string_view msg) {
            std::lock_guard lock(m_mutex);
            EnsureOpen();
            if (!m_stream.is_open()) return;
            m_stream << FormatTimestamp() << " [" << level << "] " << msg << '\n';
            m_stream.flush();
        }

        void Flush() {
            std::lock_guard lock(m_mutex);
            if (m_stream.is_open()) {
                m_stream.flush();
            }
        }

        ~FileSink() {
            if (m_stream.is_open()) {
                m_stream.flush();
            }
        }

    private:
        FileSink() = default;

        void EnsureOpen() {
            if (!m_stream.is_open()) {
                m_stream.open("czn_ripper.log", std::ios::app);
            }
        }

        static std::string FormatTimestamp() {
            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm local_tm{};
#ifdef _WIN32
            localtime_s(&local_tm, &t);
#else
            localtime_r(&t, &local_tm);
#endif
            return std::format(
                "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
                local_tm.tm_year + 1900,
                local_tm.tm_mon + 1,
                local_tm.tm_mday,
                local_tm.tm_hour,
                local_tm.tm_min,
                local_tm.tm_sec);
        }

        std::mutex m_mutex;
        std::ofstream m_stream;
    };

    inline void Info(std::string_view message) {
        FileSink::Instance().Write("INFO", message);
    }

    inline void Error(std::string_view message) {
        FileSink::Instance().Write("ERROR", message);
    }

    inline void Flush() {
        FileSink::Instance().Flush();
    }

} // namespace Logger

inline void LogInfo(std::string_view message) {
    Logger::Info(message);
}

inline void LogError(std::string_view message) {
    Logger::Error(message);
}

inline void LogFlush() {
    Logger::Flush();
}
