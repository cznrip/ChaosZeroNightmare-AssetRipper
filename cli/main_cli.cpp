#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "archive/ArchiveFactory.hpp"
#include "archive/IArchive.hpp"
#include "core/AppInfo.hpp"
#include "core/Core.hpp"
#include "core/FileTree.hpp"
#include "core/Logger.hpp"
#include "core/ProgressReporter.hpp"
#include "services/AssetExporter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <shellapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

    /** Exit codes allowing scripts to distinguish partial vs. complete failure. */
    enum class ExitCode : int {
        Ok = 0,
        Usage = 1,
        PackFailed = 2,
        PartialFailure = 3,
        AllFailed = 4
    };

    enum class ParseStatus {
        Ok,
        HelpPrinted,
        Error
    };

    /** Parsed command-line configuration for a single CLI invocation. */
    struct CliOptions {
        std::string pack;
        std::string out;
        std::vector<std::string> folders;
        bool convert_sct_to_png = true;
        bool convert_db_to_json = true;
        bool verbose = false;
        bool quiet = false;
    };

    /** One requested archive node resolved before extraction starts. */
    struct ResolvedTarget {
        const Core::FileNode* node = nullptr;
        uint64_t bytes = 0;
        uint32_t files = 0;
    };

    /**
     * Parses CLI arguments (supporting UTF-8 on Windows via CommandLineToArgvW)
     * and validates required flags.
     */
    class CliArgParser {
    public:
        static std::vector<std::string> CollectArgsUtf8(int argc, char** argv) {
            std::vector<std::string> args;
#ifdef _WIN32
            int wargc = 0;
            if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc); wargv != nullptr) {
                args.reserve(static_cast<size_t>((std::max)(0, wargc - 1)));
                for (int i = 1; i < wargc; ++i) {
                    args.push_back(Core::WStringToUtf8(wargv[i]));
                }
                LocalFree(wargv);
                return args;
            }
#endif
            args.reserve(static_cast<size_t>((std::max)(0, argc - 1)));
            for (int i = 1; i < argc; ++i) {
                args.emplace_back(argv[i]);
            }
            return args;
        }

        static void PrintHelp() {
            std::cout <<
                "ChaosZeroNightmareRipper-CLI - headless extractor for Chaos Zero Nightmare archives\n"
                "\n"
                "Usage:\n"
                "  czn-cli --pack <path> --out <dir> --folder <archive/path> [--folder ...] [options]\n"
                "\n"
                "Required:\n"
                "  -p, --pack <path>      data.pack, manifest.ssra, or an unpacked directory\n"
                "  -o, --out <dir>        destination directory (created if missing)\n"
                "  -f, --folder <path>    archive-relative folder or file to extract; repeatable\n"
                "\n"
                "Options:\n"
                "      --no-png           keep .sct / .sct2 as-is instead of converting to .png\n"
                "      --no-json          keep .db as-is instead of converting to .json\n"
                "  -v, --verbose          print each extracted file\n"
                "  -q, --quiet            suppress the progress bar; print only the summary\n"
                "  -h, --help             show this help and exit\n"
                "\n"
                "Notes:\n"
                "  Output preserves the full archive path. --out D:\\out --folder gameres/spine\n"
                "  writes D:\\out\\gameres\\spine\\...\n"
                "  .scsp files are always converted to .json.\n"
                "  Settings in czn_ripper.ini are ignored. Use the flags above.\n"
                "  Prefer a short destination path. Deep archive paths can exceed the Windows\n"
                "  MAX_PATH limit when the destination is already long.\n"
                "\n"
                "Exit codes report whether each requested folder was FOUND and extracted. Errors\n"
                "on individual files are logged and skipped without changing the exit code, so\n"
                "check czn_ripper.log in the working directory to confirm a clean run.\n"
                "  0  every requested folder was found and extracted\n"
                "  1  usage error\n"
                "  2  pack could not be opened or scanned\n"
                "  3  some requested folders were missing or failed\n"
                "  4  none of the requested folders could be extracted\n"
                "\n"
                "Example:\n"
                "  ChaosZeroNightmareRipper-CLI --pack \"C:\\Games\\ChaosZeroNightmare\\bin\\appdata\\cznlive\\data.pack\"\n"
                "          --out D:\\czn_assets\n"
                "          --folder card --folder story --folder cutin --folder collapse\n";
        }

        static ParseStatus Parse(std::span<const std::string> args, CliOptions& out_options) {
            std::unordered_set<std::string> seen_folders;

            for (size_t i = 0; i < args.size(); ++i) {
                std::string_view arg = args[i];
                std::string_view inline_value;
                bool has_inline = false;

                if (arg.starts_with("--")) {
                    if (const size_t equals = arg.find('='); equals != std::string_view::npos) {
                        inline_value = arg.substr(equals + 1);
                        arg = arg.substr(0, equals);
                        has_inline = true;
                    }
                }

                auto take_value = [&](std::string_view flag_name, std::string& dest) -> bool {
                    if (has_inline) {
                        dest = std::string(inline_value);
                    } else if (i + 1 < args.size()) {
                        dest = args[++i];
                    } else {
                        std::cerr << std::format("error: {} needs a value\n", flag_name);
                        return false;
                    }

                    if (dest.empty()) {
                        std::cerr << std::format("error: {} needs a non-empty value\n", flag_name);
                        return false;
                    }
                    return true;
                };

                auto reject_value = [&]() -> bool {
                    if (!has_inline) return true;
                    std::cerr << std::format("error: {} does not take a value\n", arg);
                    return false;
                };

                if (arg == "-h" || arg == "--help") {
                    if (!reject_value()) return ParseStatus::Error;
                    PrintHelp();
                    return ParseStatus::HelpPrinted;
                }
                if (arg == "-p" || arg == "--pack") {
                    if (!take_value("--pack", out_options.pack)) return ParseStatus::Error;
                } else if (arg == "-o" || arg == "--out") {
                    if (!take_value("--out", out_options.out)) return ParseStatus::Error;
                } else if (arg == "-f" || arg == "--folder") {
                    std::string raw;
                    if (!take_value("--folder", raw)) return ParseStatus::Error;

                    const std::string normalized = Core::NormalizeArchivePath(raw);
                    if (normalized.empty()) {
                        std::cerr << std::format(
                            "error: whole-archive extraction is not supported; name a folder (got \"{}\")\n",
                            raw);
                        return ParseStatus::Error;
                    }
                    if (seen_folders.insert(normalized).second) {
                        out_options.folders.push_back(normalized);
                    }
                } else if (arg == "--no-png") {
                    if (!reject_value()) return ParseStatus::Error;
                    out_options.convert_sct_to_png = false;
                } else if (arg == "--no-json") {
                    if (!reject_value()) return ParseStatus::Error;
                    out_options.convert_db_to_json = false;
                } else if (arg == "-v" || arg == "--verbose") {
                    if (!reject_value()) return ParseStatus::Error;
                    out_options.verbose = true;
                } else if (arg == "-q" || arg == "--quiet") {
                    if (!reject_value()) return ParseStatus::Error;
                    out_options.quiet = true;
                } else {
                    std::cerr << std::format("error: unrecognized argument \"{}\"\n", args[i]);
                    return ParseStatus::Error;
                }
            }

            if (out_options.pack.empty()) {
                std::cerr << "error: --pack is required\n";
                return ParseStatus::Error;
            }
            if (out_options.out.empty()) {
                std::cerr << "error: --out is required\n";
                return ParseStatus::Error;
            }
            if (out_options.folders.empty()) {
                std::cerr << "error: at least one --folder is required\n";
                return ParseStatus::Error;
            }
            return ParseStatus::Ok;
        }
    };

    /**
     * Renders throttled in-place progress bars on TTY consoles or sparse 5%-step
     * lines when stdout is redirected, driven directly by Core::ProgressReporter.
     */
    class ConsoleProgressBar {
    public:
        static constexpr int kBarWidth = 30;
        static constexpr int kPipeStepPercent = 5;
        static constexpr auto kTtyThrottleInterval = std::chrono::milliseconds(50);

        ConsoleProgressBar(bool is_tty, bool quiet) noexcept
            : m_is_tty(is_tty), m_quiet(quiet) {}

        Core::ProgressReporter MakeReporter(
            std::string label,
            std::function<std::string(float)> suffix_fn = {})
        {
            Reset();
            return Core::ProgressReporter::FromCallback(
                [this, label = std::move(label), suffix_fn = std::move(suffix_fn)](float fraction) {
                    const std::string suffix = suffix_fn ? suffix_fn(fraction) : std::string{};
                    Draw(label, fraction, suffix, false);
                });
        }

        void Finish(std::string_view label, std::string_view suffix = {}) {
            if (m_quiet) return;
            Draw(label, 1.0f, suffix, true);
            if (m_is_tty) {
                std::cout << '\n';
            }
        }

    private:
        void Reset() noexcept {
            m_last_percent = -1;
            m_last_draw = Clock::time_point{};
        }

        void Draw(std::string_view label, float fraction, std::string_view suffix, bool force) {
            if (m_quiet) return;

            fraction = std::clamp(fraction, 0.0f, 1.0f);
            const int percent = static_cast<int>(fraction * 100.0f + 0.5f);

            if (!m_is_tty) {
                if (percent == m_last_percent) return;
                if (!force && (percent % kPipeStepPercent) != 0) return;
                m_last_percent = percent;

                if (suffix.empty()) {
                    std::cout << std::format("{} {}%\n", label, percent);
                } else {
                    std::cout << std::format("{} {}% {}\n", label, percent, suffix);
                }
                return;
            }

            const auto now = Clock::now();
            if (!force && m_last_percent >= 0 && percent == m_last_percent &&
                (now - m_last_draw) < kTtyThrottleInterval) {
                return;
            }
            m_last_percent = percent;
            m_last_draw = now;

            const int filled = static_cast<int>(fraction * kBarWidth + 0.5f);
            std::string bar(static_cast<size_t>(filled), '#');
            bar.append(static_cast<size_t>(kBarWidth - filled), '-');

            std::cout << std::format("\r{} [{}] {:3d}%  {}        ", label, bar, percent, suffix)
                      << std::flush;
        }

        using Clock = std::chrono::steady_clock;

        bool m_is_tty = false;
        bool m_quiet = false;
        int m_last_percent = -1;
        Clock::time_point m_last_draw{};
    };

    /**
     * Headless CLI application orchestrating archive opening, scanning, target
     * resolution, and asset extraction via ArchiveFactory and AssetExporter.
     */
    class CliApp {
    public:
        int Run(int argc, char** argv) {
            const bool is_tty = ConfigureConsole();

            CliOptions options;
            const auto args = CliArgParser::CollectArgsUtf8(argc, argv);
            const ParseStatus status = CliArgParser::Parse(args, options);
            if (status == ParseStatus::HelpPrinted) {
                return static_cast<int>(ExitCode::Ok);
            }
            if (status != ParseStatus::Ok) {
                std::cerr << "\nRun czn-cli --help for usage.\n";
                return static_cast<int>(ExitCode::Usage);
            }

            const std::filesystem::path pack_path(Core::Utf8ToWString(options.pack));
            const std::filesystem::path dest_path(Core::Utf8ToWString(options.out));

            if (!std::filesystem::exists(pack_path)) {
                std::cerr << std::format("error: pack not found: {}\n", options.pack);
                return static_cast<int>(ExitCode::PackFailed);
            }

            try {
                std::filesystem::create_directories(dest_path);
            } catch (const std::exception& e) {
                std::cerr << std::format("error: could not create output directory {}: {}\n", options.out, e.what());
                return static_cast<int>(ExitCode::PackFailed);
            }

            LogInfo("CLI run started for pack: " + options.pack);

            std::unique_ptr<IArchive> archive;
            if (!RunGuarded([&] { archive = ArchiveFactory::Open(pack_path); }) || !archive) {
                std::cerr << std::format("error: could not open pack: {}\n", options.pack);
                return static_cast<int>(ExitCode::PackFailed);
            }

            std::cout << std::format("Pack:        {}\n", options.pack);
            std::cout << std::format("Destination: {}\n", Core::PathToUtf8(std::filesystem::absolute(dest_path)));
            std::cout << "Scanning archive...\n";

            ConsoleProgressBar progress_bar(is_tty, options.quiet);
            auto scan_reporter = progress_bar.MakeReporter("Scanning");
            if (!RunGuarded([&] { archive->Scan(scan_reporter); })) {
                std::cerr << "error: scan failed\n";
                return static_cast<int>(ExitCode::PackFailed);
            }
            progress_bar.Finish("Scanning");

            std::cout << std::format(
                "Scanned {} files ({})\n\n",
                archive->GetParsedFileCount(),
                Core::FormatSize(archive->GetParsedTotalSize()));

            // Resolve every requested folder upfront so missing paths are reported immediately.
            const Core::FileNode& root = archive->GetFileTree();
            std::vector<ResolvedTarget> resolved;
            std::vector<std::string> missing;
            uint64_t bytes_total = 0;

            resolved.reserve(options.folders.size());
            for (const std::string& folder : options.folders) {
                const Core::FileNode* node = Core::FindNodeByPath(root, folder);
                if (!node) {
                    std::cerr << std::format("error: not found in archive: {}\n", folder);
                    LogError("CLI folder not found in archive: " + folder);
                    missing.push_back(folder);
                    continue;
                }

                uint64_t bytes = 0;
                uint32_t files = 0;
                Core::NodeStats(*node, bytes, files);
                bytes_total += bytes;
                resolved.push_back({node, bytes, files});
            }

            if (resolved.empty()) {
                std::cerr << "\nNothing to extract.\n";
                return static_cast<int>(ExitCode::AllFailed);
            }

            const ExportOptions export_opts{options.convert_sct_to_png, options.convert_db_to_json};
            const AssetExporter exporter;
            std::vector<std::string> failed;
            uint64_t bytes_done = 0;

            for (size_t i = 0; i < resolved.size(); ++i) {
                const ResolvedTarget& target = resolved[i];
                const std::string& folder_path = target.node->full_path;

                std::cout << std::format(
                    "[{}/{}] {}  ({} files, {})\n",
                    i + 1,
                    resolved.size(),
                    folder_path,
                    target.files,
                    Core::FormatSize(target.bytes));

                if (target.bytes == 0) {
                    std::cout << "  warning: no data to extract\n";
                    LogInfo("CLI folder holds no data: " + folder_path);
                    continue;
                }

                const std::filesystem::path out_base = OutputBaseFor(dest_path, folder_path);
                bool ok = true;

                if (options.verbose) {
                    uint32_t files_done = 0;
                    ok = RunGuarded([&] {
                        exporter.ExtractNode(
                            *archive,
                            *target.node,
                            out_base,
                            export_opts,
                            Core::ProgressReporter::Null(),
                            [&](const Core::FileNode& extracted) {
                                ++files_done;
                                const auto& info = std::get<Core::FileInfo>(extracted.data);
                                std::cout << std::format(
                                    "  [{}/{}] {}  ({})\n",
                                    files_done,
                                    target.files,
                                    extracted.full_path,
                                    Core::FormatSize(info.size));
                            });
                    });
                } else {
                    auto make_suffix = [bytes_done, bytes_total, folder_bytes = target.bytes](float local) {
                        const float clamped = std::clamp(local, 0.0f, 1.0f);
                        const uint64_t overall_done =
                            bytes_done + static_cast<uint64_t>(clamped * static_cast<double>(folder_bytes));
                        const int total_percent = (bytes_total > 0)
                            ? static_cast<int>(100.0 * static_cast<double>(overall_done) / static_cast<double>(bytes_total))
                            : 100;
                        return std::format("total {}%", total_percent);
                    };

                    auto extract_reporter = progress_bar.MakeReporter("  Extracting", make_suffix);
                    ok = RunGuarded([&] {
                        exporter.ExtractNode(*archive, *target.node, out_base, export_opts, extract_reporter);
                    });
                    progress_bar.Finish("  Extracting", make_suffix(1.0f));
                }

                bytes_done += target.bytes;
                if (!ok) {
                    failed.push_back(folder_path);
                }
            }

            const size_t failure_count = missing.size() + failed.size();
            const size_t success_count = options.folders.size() - failure_count;

            std::cout << std::format("\nDone. {} of {} folders extracted.\n", success_count, options.folders.size());
            if (!missing.empty()) {
                std::cout << "Not found in archive:\n";
                for (const std::string& folder : missing) {
                    std::cout << std::format("  {}\n", folder);
                }
            }
            if (!failed.empty()) {
                std::cout << "Failed during extraction:\n";
                for (const std::string& folder : failed) {
                    std::cout << std::format("  {}\n", folder);
                }
            }

            LogInfo(std::format("CLI run finished: {}/{} folders extracted", success_count, options.folders.size()));
            std::cout << std::format(
                "Per-file log: {}\n",
                Core::PathToUtf8(std::filesystem::absolute("czn_ripper.log")));

            if (failure_count == 0) return static_cast<int>(ExitCode::Ok);
            if (success_count == 0) return static_cast<int>(ExitCode::AllFailed);
            return static_cast<int>(ExitCode::PartialFailure);
        }

    private:
        static bool ConfigureConsole() {
#ifdef _WIN32
            SetConsoleOutputCP(CP_UTF8);
            const bool is_tty = _isatty(_fileno(stdout)) != 0;
#else
            const bool is_tty = isatty(1) != 0;
#endif
            // Piped stdout is fully buffered; flush after every write so callers see live progress.
            if (!is_tty) {
                std::cout << std::unitbuf;
            }
            return is_tty;
        }

        static std::filesystem::path OutputBaseFor(
            const std::filesystem::path& dest,
            std::string_view node_full_path)
        {
            const size_t slash = node_full_path.find_last_of('/');
            if (slash == std::string_view::npos) {
                return dest;
            }
            return dest / std::filesystem::path(Core::Utf8ToWString(node_full_path.substr(0, slash)));
        }

        template <typename Fn>
        static bool RunGuarded(Fn&& work) {
            try {
                work();
                return true;
            } catch (const std::exception& e) {
                std::cerr << std::format("  error: {}\n", e.what());
                LogError(std::string("CLI operation failed: ") + e.what());
                return false;
            } catch (...) {
                std::cerr << "  error: unknown exception\n";
                LogError("CLI operation failed with an unknown exception");
                return false;
            }
        }
    };

} // namespace

int main(int argc, char** argv) {
    CliApp app;
    return app.Run(argc, argv);
}
