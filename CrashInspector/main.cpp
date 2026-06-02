#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwchar>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr size_t MaxReadBytes = 12ull * 1024ull * 1024ull;
constexpr size_t MaxFilesToRead = 120;

struct Options
{
    std::vector<fs::path> roots;
    std::optional<std::string> gameFilter;
    bool watch = false;
    bool noReport = false;
    bool openReport = false;
    bool help = false;
    int contextBefore = 50;
    int contextAfter = 25;
};

enum class FileKind
{
    Log,
    Diagnostics,
    CrashContext,
    Text,
};

struct CandidateFile
{
    fs::path path;
    FileKind kind = FileKind::Text;
    fs::file_time_type modified{};
};

struct SourceFile
{
    CandidateFile candidate;
    std::string text;
    std::vector<std::string> lines;
    int score = 0;
};

struct Evidence
{
    fs::path path;
    int line = 0;
    std::string text;
};

struct Pattern
{
    std::string needle;
    int weight = 0;
};

struct Rule
{
    std::string id;
    std::string title;
    std::string plainEnglish;
    std::vector<Pattern> patterns;
    std::vector<std::string> nextSteps;
};

struct RuleHit
{
    const Rule* rule = nullptr;
    int score = 0;
    std::vector<Evidence> evidence;
};

struct AnalysisResult
{
    bool hasCrash = false;
    std::vector<SourceFile> sources;
    std::vector<Evidence> markers;
    std::vector<Evidence> logContext;
    std::vector<std::string> callstack;
    std::vector<RuleHit> ruleHits;
    fs::path reportPath;
    std::vector<std::string> warnings;
};

std::optional<fs::path> GetLocalAppDataPath();

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};

    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};

    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0)
        return {};

    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), needed);
    return result;
}

std::string PathString(const fs::path& path)
{
    return WideToUtf8(path.wstring());
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string Trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;

    return value.substr(begin, end - begin);
}

bool ContainsCi(const std::string& haystack, const std::string& needle)
{
    return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

bool EndsWithCi(const std::string& value, const std::string& suffix)
{
    const std::string lowerValue = ToLower(value);
    const std::string lowerSuffix = ToLower(suffix);
    if (lowerSuffix.size() > lowerValue.size())
        return false;

    return lowerValue.compare(lowerValue.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
}

std::string Shorten(std::string value, size_t limit)
{
    value = Trim(value);
    if (value.size() <= limit)
        return value;

    if (limit <= 3)
        return value.substr(0, limit);

    return value.substr(0, limit - 3) + "...";
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;

    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }

    return lines;
}

std::string FileTimeToString(const fs::file_time_type& fileTime)
{
    if (fileTime == fs::file_time_type{})
        return "unknown";

    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());

    const std::time_t time = std::chrono::system_clock::to_time_t(systemTime);
    std::tm local{};
    localtime_s(&local, &time);

    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string NowForFileName()
{
    const std::time_t time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &time);

    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S");
    return out.str();
}

std::string NowForReport()
{
    const std::time_t time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &time);

    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string DecodeUtf16(const std::vector<char>& bytes, size_t offset, bool littleEndian)
{
    std::wstring wide;
    for (size_t i = offset; i + 1 < bytes.size(); i += 2)
    {
        const unsigned char first = static_cast<unsigned char>(bytes[i]);
        const unsigned char second = static_cast<unsigned char>(bytes[i + 1]);
        const wchar_t codeUnit = littleEndian
            ? static_cast<wchar_t>(first | (second << 8))
            : static_cast<wchar_t>((first << 8) | second);
        wide.push_back(codeUnit);
    }

    return WideToUtf8(wide);
}

bool LooksUtf16Le(const std::vector<char>& bytes)
{
    const size_t sample = std::min<size_t>(bytes.size(), 512);
    if (sample < 8)
        return false;

    size_t nulOdd = 0;
    size_t checkedOdd = 0;
    for (size_t i = 1; i < sample; i += 2)
    {
        ++checkedOdd;
        if (bytes[i] == '\0')
            ++nulOdd;
    }

    return checkedOdd > 0 && nulOdd * 100 / checkedOdd > 60;
}

std::string DecodeBytes(const std::vector<char>& bytes)
{
    if (bytes.size() >= 2)
    {
        const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
        const unsigned char b1 = static_cast<unsigned char>(bytes[1]);
        if (b0 == 0xff && b1 == 0xfe)
            return DecodeUtf16(bytes, 2, true);
        if (b0 == 0xfe && b1 == 0xff)
            return DecodeUtf16(bytes, 2, false);
    }

    size_t offset = 0;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf)
    {
        offset = 3;
    }

    if (LooksUtf16Le(bytes))
        return DecodeUtf16(bytes, 0, true);

    std::string text(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
    return text;
}

std::optional<std::string> ReadTextFile(const fs::path& path, std::vector<std::string>& warnings)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec)
    {
        warnings.push_back("Could not read size for " + PathString(path) + ": " + ec.message());
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        warnings.push_back("Could not open " + PathString(path));
        return std::nullopt;
    }

    const bool clipped = size > MaxReadBytes;
    if (clipped)
        file.seekg(static_cast<std::streamoff>(size - MaxReadBytes), std::ios::beg);

    std::vector<char> bytes(static_cast<size_t>(std::min<uintmax_t>(size, MaxReadBytes)));
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(file.gcount()));

    std::string text = DecodeBytes(bytes);
    if (clipped)
    {
        const size_t firstBreak = text.find('\n');
        if (firstBreak != std::string::npos)
            text.erase(0, firstBreak + 1);
        text.insert(0, "[older content omitted because the file is large]\n");
    }

    return text;
}

FileKind GuessKind(const fs::path& path)
{
    const std::string name = ToLower(PathString(path.filename()));
    const std::string ext = ToLower(PathString(path.extension()));

    if (name == "diagnostics.txt")
        return FileKind::Diagnostics;
    if (name == "crashcontext.runtime-xml" || (name.find("crashcontext") != std::string::npos && ext == ".xml"))
        return FileKind::CrashContext;
    if (ext == ".log")
        return FileKind::Log;
    return FileKind::Text;
}

bool IsCandidateFile(const fs::path& path)
{
    const std::string name = ToLower(PathString(path.filename()));
    const std::string ext = ToLower(PathString(path.extension()));

    return ext == ".log" ||
        name == "diagnostics.txt" ||
        name == "crashcontext.runtime-xml" ||
        (name.find("crashcontext") != std::string::npos && ext == ".xml") ||
        (name.find("diagnostic") != std::string::npos && ext == ".txt");
}

bool ShouldSkipDirectory(const fs::path& path)
{
    const std::string name = ToLower(PathString(path.filename()));
    static const std::set<std::string> skipped = {
        ".git", ".vs", ".vscode", "node_modules", "imgui-master", "intermediate",
        "intermediates", "binaries", "deriveddatacache", "savedwebcache", "out"
    };

    return skipped.contains(name);
}

bool MatchesGameFilter(const fs::path& path, const std::optional<std::string>& filter)
{
    if (!filter)
        return true;

    return ContainsCi(PathString(path), *filter);
}

void AddCandidate(const fs::path& path, std::vector<CandidateFile>& candidates, std::set<std::wstring>& seen)
{
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    const fs::path stable = ec ? fs::absolute(path, ec) : canonical;
    const std::wstring key = stable.wstring();
    if (seen.contains(key))
        return;

    CandidateFile file;
    file.path = stable;
    file.kind = GuessKind(stable);
    file.modified = fs::last_write_time(stable, ec);
    if (ec)
        file.modified = fs::file_time_type{};

    candidates.push_back(file);
    seen.insert(key);
}

void ScanRecursive(const fs::path& root, int depth, const Options& options, std::vector<CandidateFile>& candidates, std::set<std::wstring>& seen)
{
    if (depth < 0 || candidates.size() > 2000)
        return;

    std::error_code ec;
    if (!fs::exists(root, ec))
        return;

    if (fs::is_regular_file(root, ec))
    {
        if (IsCandidateFile(root) && MatchesGameFilter(root, options.gameFilter))
            AddCandidate(root, candidates, seen);
        return;
    }

    if (!fs::is_directory(root, ec))
        return;

    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::directory_entry entry = *it;
        if (entry.is_directory(ec))
        {
            if (!ShouldSkipDirectory(entry.path()))
                ScanRecursive(entry.path(), depth - 1, options, candidates, seen);
        }
        else if (entry.is_regular_file(ec))
        {
            if (IsCandidateFile(entry.path()) && MatchesGameFilter(entry.path(), options.gameFilter))
                AddCandidate(entry.path(), candidates, seen);
        }
    }
}

void ScanKnownUnrealLayout(const fs::path& root, const Options& options, std::vector<CandidateFile>& candidates, std::set<std::wstring>& seen)
{
    std::error_code ec;
    if (!fs::exists(root, ec))
        return;

    if (fs::is_regular_file(root, ec))
    {
        AddCandidate(root, candidates, seen);
        return;
    }

    const std::vector<fs::path> likelyRoots = {
        root,
        root / "Saved" / "Logs",
        root / "Saved" / "Crashes",
        root / "Logs",
        root / "Crashes"
    };

    for (const fs::path& path : likelyRoots)
        ScanRecursive(path, 4, options, candidates, seen);

    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::directory_entry entry = *it;
        if (!entry.is_directory(ec) || ShouldSkipDirectory(entry.path()))
            continue;

        ScanRecursive(entry.path() / "Saved" / "Logs", 2, options, candidates, seen);
        ScanRecursive(entry.path() / "Saved" / "Crashes", 4, options, candidates, seen);
    }
}

std::vector<CandidateFile> DiscoverCandidates(const Options& options)
{
    std::vector<CandidateFile> candidates;
    std::set<std::wstring> seen;

    const bool hasExplicitRoots = !options.roots.empty();
    std::vector<fs::path> roots = options.roots;
    if (roots.empty())
        roots.push_back(fs::current_path());

    for (const fs::path& root : roots)
        ScanKnownUnrealLayout(root, options, candidates, seen);

    if (auto localAppData = GetLocalAppDataPath(); !hasExplicitRoots && localAppData)
    {
        const fs::path localRoot = *localAppData;
        std::error_code ec;
        for (fs::directory_iterator it(localRoot, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::directory_entry entry = *it;
            if (!entry.is_directory(ec))
                continue;

            if (!MatchesGameFilter(entry.path(), options.gameFilter))
                continue;

            ScanRecursive(entry.path() / "Saved" / "Logs", 2, options, candidates, seen);
            ScanRecursive(entry.path() / "Saved" / "Crashes", 5, options, candidates, seen);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const CandidateFile& lhs, const CandidateFile& rhs) {
        return lhs.modified > rhs.modified;
    });

    return candidates;
}

std::vector<Rule> BuildRules()
{
    return {
        {
            "gpu",
            "GPU or graphics device crash",
            "The crash points at the render device or driver. Unreal often reports this as a generic fatal error unless you read the log or crash context.",
            {
                {"gpu crashed", 35},
                {"dxgi_error_device_removed", 35},
                {"dxgi_error_device_hung", 35},
                {"d3d device being lost", 30},
                {"device removed", 24},
                {"d3d12rhi", 14},
                {"d3d11rhi", 10},
                {"vulkanrhi", 10},
                {"nvwgf2umx", 18},
                {"atidxx", 18},
                {"amdxx", 18}
            },
            {
                "Try a DirectX mode switch, usually -dx11 or -d3d11 for UE4/UE5 games.",
                "Update or roll back the GPU driver, then clear the game's shader cache if it has one.",
                "Disable overlays, frame generation, overclocks, ReShade, and aggressive GPU tuning for a clean test."
            }
        },
        {
            "memory",
            "Out of memory or VRAM exhaustion",
            "The game ran out of address space, system RAM, VRAM, or page file capacity while Unreal was loading or rendering content.",
            {
                {"out of memory", 35},
                {"ran out of memory", 35},
                {"out of video memory", 34},
                {"not enough memory", 28},
                {"could not allocate", 22},
                {"bad allocation", 22},
                {"paging file", 18},
                {"requested allocation size", 18}
            },
            {
                "Lower texture quality and resolution for a quick validation pass.",
                "Close memory-heavy apps and make sure the Windows page file is enabled.",
                "If the crash happens during loading, verify game files because corrupt content can trigger huge failed allocations."
            }
        },
        {
            "null_pointer",
            "Null object access",
            "The strongest clue is an access violation at or near address zero. That usually means code tried to use an Unreal object that was missing, destroyed, or never initialized.",
            {
                {"access violation reading address 0x0000000000000000", 40},
                {"access violation writing address 0x0000000000000000", 40},
                {"exception_access_violation reading address 0x0000000000000000", 40},
                {"exception_access_violation writing address 0x0000000000000000", 40},
                {"nullptr", 18},
                {"null pointer", 18},
                {"pending kill", 12}
            },
            {
                "Use the top non-system call stack frame to find the feature, plugin, or asset involved.",
                "For your own project, add a validity check before using the object that appears in the stack.",
                "If this is a packaged game, remove recent mods/plugins and verify files."
            }
        },
        {
            "access_violation",
            "Native access violation",
            "Unreal crashed in native code with a memory access fault. The call stack and module names are the most important clues.",
            {
                {"exception_access_violation", 28},
                {"unhandled exception: 0xc0000005", 28},
                {"c0000005", 22},
                {"access violation", 22}
            },
            {
                "Read the top non-Windows call stack frame; that module is usually the best starting point.",
                "Remove mods, injected overlays, or outdated plugins for a clean repro.",
                "If it is your code, reproduce under Visual Studio or Rider with symbols enabled."
            }
        },
        {
            "assertion",
            "Assertion or check failed",
            "The game hit a deliberate Unreal check. The assertion text is usually the real reason, not the generic fatal error window.",
            {
                {"assertion failed", 36},
                {"check failed", 30},
                {"lowlevelfatalerror", 20},
                {"fatal error:", 14}
            },
            {
                "Read the assertion line exactly; it names the condition Unreal expected to be true.",
                "Look 20 to 50 log lines above the fatal error for the asset, map, or subsystem that triggered it.",
                "For source projects, run the same build configuration under the debugger to break at the failing check."
            }
        },
        {
            "ensure",
            "Ensure condition failed",
            "An Unreal ensure fired. Ensures can be recoverable in editor builds, but packaged games may still close if the surrounding code cannot continue.",
            {
                {"ensure condition failed", 30},
                {"handled ensure", 18}
            },
            {
                "Inspect the ensure message and the function above it in the call stack.",
                "If the game uses mods or plugins, test without them because ensures often expose invalid assumptions.",
                "For your own project, add logging around the objects named immediately before the ensure."
            }
        },
        {
            "missing_asset",
            "Missing, corrupt, or incompatible asset",
            "The log indicates Unreal could not load a package, asset, blueprint, map, shader, or pak entry it expected to exist.",
            {
                {"couldn't find file", 30},
                {"can't find file", 30},
                {"failed to load", 24},
                {"failed to find", 22},
                {"missing package", 26},
                {"can't find package", 26},
                {"could not find package", 26},
                {"failed import", 22},
                {"verifyimport", 20},
                {"linkerload", 12},
                {"corrupt pak", 24},
                {"pak file", 12}
            },
            {
                "Verify or repair the game install so missing pak and asset files are restored.",
                "Remove recently added mods, loose files, or incompatible cooked assets.",
                "If this is your packaged project, confirm the referenced asset is included in cooking and packaging."
            }
        },
        {
            "blueprint_loop",
            "Blueprint infinite loop",
            "Unreal detected runaway Blueprint execution. This often closes packaged builds without a useful UI message.",
            {
                {"infinite loop detected", 40},
                {"runaway loop", 35},
                {"maximum loop iteration count", 25}
            },
            {
                "Open the Blueprint named near the error and inspect loops, Tick logic, and recursive events.",
                "Add a delay or exit condition to the loop and retest.",
                "For packaged games, report the Blueprint/function name from this report to the developer."
            }
        },
        {
            "plugin",
            "Plugin, module, or mod load failure",
            "The crash evidence names a plugin, module, or mod path. This usually means a version mismatch, missing binary, or incompatible injected component.",
            {
                {".uplugin", 24},
                {"plugin", 12},
                {"modulemanager", 18},
                {"module could not be loaded", 28},
                {"missing import", 18},
                {"mods", 10},
                {"mod ", 8}
            },
            {
                "Disable third-party plugins, mods, overlays, and injected tools, then retry.",
                "Make sure the plugin was built for the exact Unreal/game version.",
                "Verify that the Win64 plugin DLL exists beside the packaged game binary."
            }
        },
        {
            "permissions",
            "File path or permission problem",
            "The fatal error appears tied to Windows file access, permissions, or a missing writable directory.",
            {
                {"access is denied", 30},
                {"access denied", 30},
                {"permission denied", 28},
                {"failed to create file", 22},
                {"failed to open file", 20},
                {"path not found", 18},
                {"sharing violation", 18}
            },
            {
                "Run once from a normal writable install path, not a protected Program Files or synced folder path.",
                "Check antivirus quarantine and controlled folder access.",
                "Delete stale config/cache files only for the affected game after backing up saves."
            }
        }
    };
}

std::string XmlDecode(std::string value)
{
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&amp;", "&"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&#xA;", "\n"},
        {"&#10;", "\n"},
        {"&#xD;", "\r"},
        {"&#13;", "\r"}
    };

    for (const auto& [from, to] : replacements)
    {
        size_t pos = 0;
        while ((pos = value.find(from, pos)) != std::string::npos)
        {
            value.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    return value;
}

std::optional<std::string> ExtractXmlTag(const std::string& text, const std::string& tag)
{
    const std::string lower = ToLower(text);
    const std::string open = "<" + ToLower(tag) + ">";
    const std::string close = "</" + ToLower(tag) + ">";

    size_t begin = lower.find(open);
    if (begin == std::string::npos)
        return std::nullopt;

    begin += open.size();
    const size_t end = lower.find(close, begin);
    if (end == std::string::npos || end <= begin)
        return std::nullopt;

    std::string value = text.substr(begin, end - begin);
    const std::string cdataOpen = "<![CDATA[";
    const std::string cdataClose = "]]>";
    const size_t cdataBegin = value.find(cdataOpen);
    const size_t cdataEnd = value.rfind(cdataClose);
    if (cdataBegin != std::string::npos && cdataEnd != std::string::npos && cdataEnd > cdataBegin)
        value = value.substr(cdataBegin + cdataOpen.size(), cdataEnd - (cdataBegin + cdataOpen.size()));

    return Trim(XmlDecode(value));
}

std::vector<std::string> MarkerNeedles()
{
    return {
        "fatal error:",
        "lowlevelfatalerror",
        "assertion failed",
        "check failed",
        "ensure condition failed",
        "unhandled exception",
        "exception_access_violation",
        "gpu crashed",
        "dxgi_error_device_removed",
        "dxgi_error_device_hung",
        "out of memory",
        "crashcontext"
    };
}

int ScoreSource(const SourceFile& source)
{
    int score = 0;
    switch (source.candidate.kind)
    {
    case FileKind::CrashContext:
        score += 18;
        break;
    case FileKind::Diagnostics:
        score += 16;
        break;
    case FileKind::Log:
        score += 8;
        break;
    case FileKind::Text:
        score += 2;
        break;
    }

    const std::string lowerText = ToLower(source.text);
    for (const std::string& needle : MarkerNeedles())
    {
        if (lowerText.find(needle) != std::string::npos)
            score += 12;
    }

    return score;
}

std::vector<SourceFile> ReadSources(const std::vector<CandidateFile>& candidates, std::vector<std::string>& warnings)
{
    std::vector<SourceFile> sources;
    const size_t count = std::min(candidates.size(), MaxFilesToRead);

    for (size_t i = 0; i < count; ++i)
    {
        auto text = ReadTextFile(candidates[i].path, warnings);
        if (!text)
            continue;

        SourceFile source;
        source.candidate = candidates[i];
        source.text = std::move(*text);
        source.lines = SplitLines(source.text);
        source.score = ScoreSource(source);
        sources.push_back(std::move(source));
    }

    std::sort(sources.begin(), sources.end(), [](const SourceFile& lhs, const SourceFile& rhs) {
        if (lhs.score != rhs.score)
            return lhs.score > rhs.score;
        return lhs.candidate.modified > rhs.candidate.modified;
    });

    return sources;
}

std::vector<Evidence> GatherMarkers(const std::vector<SourceFile>& sources)
{
    std::vector<Evidence> markers;
    const std::vector<std::string> needles = MarkerNeedles();

    for (const SourceFile& source : sources)
    {
        for (size_t i = 0; i < source.lines.size(); ++i)
        {
            const std::string lowerLine = ToLower(source.lines[i]);
            for (const std::string& needle : needles)
            {
                if (lowerLine.find(needle) != std::string::npos)
                {
                    markers.push_back({ source.candidate.path, static_cast<int>(i + 1), Trim(source.lines[i]) });
                    break;
                }
            }
        }
    }

    return markers;
}

const SourceFile* FindSourceByPath(const std::vector<SourceFile>& sources, const fs::path& path)
{
    for (const SourceFile& source : sources)
    {
        if (source.candidate.path == path)
            return &source;
    }

    return nullptr;
}

std::vector<Evidence> BuildContext(const std::vector<SourceFile>& sources, const std::vector<Evidence>& markers, int before, int after)
{
    if (markers.empty())
        return {};

    const Evidence& marker = markers.front();
    const SourceFile* source = FindSourceByPath(sources, marker.path);
    if (!source)
        return {};

    const int lineIndex = std::max(0, marker.line - 1);
    const int begin = std::max(0, lineIndex - before);
    const int end = std::min<int>(static_cast<int>(source->lines.size()), lineIndex + after + 1);

    std::vector<Evidence> context;
    for (int i = begin; i < end; ++i)
        context.push_back({ source->candidate.path, i + 1, source->lines[static_cast<size_t>(i)] });

    return context;
}

bool LooksLikeStackFrame(const std::string& line)
{
    const std::string lower = ToLower(line);
    return lower.find("[callstack]") != std::string::npos ||
        lower.find("callstack") == 0 ||
        lower.find("!0x") != std::string::npos ||
        (line.find('!') != std::string::npos && (lower.find(".dll") != std::string::npos || lower.find(".exe") != std::string::npos)) ||
        (lower.find("0x") != std::string::npos && (lower.find(".dll") != std::string::npos || lower.find(".exe") != std::string::npos));
}

bool IsSystemStackFrame(const std::string& line)
{
    const std::string lower = ToLower(line);
    return lower.find("kernelbase") != std::string::npos ||
        lower.find("kernel32") != std::string::npos ||
        lower.find("ntdll") != std::string::npos ||
        lower.find("crashreportclient") != std::string::npos;
}

std::vector<std::string> ExtractCallstack(const std::vector<SourceFile>& sources)
{
    std::vector<std::string> frames;
    std::set<std::string> seen;

    auto addFrame = [&](std::string frame) {
        frame = Trim(frame);
        if (frame.empty())
            return;
        if (frame.size() > 260)
            frame = frame.substr(0, 260) + "...";
        const std::string key = ToLower(frame);
        if (seen.contains(key))
            return;
        frames.push_back(frame);
        seen.insert(key);
    };

    for (const SourceFile& source : sources)
    {
        if (source.candidate.kind == FileKind::CrashContext)
        {
            if (auto callstack = ExtractXmlTag(source.text, "CallStack"))
            {
                for (const std::string& line : SplitLines(*callstack))
                    addFrame(line);
            }
        }

        for (size_t i = 0; i < source.lines.size(); ++i)
        {
            const std::string lowerLine = ToLower(Trim(source.lines[i]));
            if (lowerLine.find("callstack") != std::string::npos || lowerLine.find("[callstack]") != std::string::npos)
            {
                for (size_t j = i; j < source.lines.size() && j < i + 60; ++j)
                {
                    const std::string line = Trim(source.lines[j]);
                    if (line.empty() && j > i + 1)
                        break;
                    if (LooksLikeStackFrame(line))
                        addFrame(line);
                }
            }
            else if (LooksLikeStackFrame(source.lines[i]))
            {
                addFrame(source.lines[i]);
            }

            if (frames.size() >= 16)
                return frames;
        }
    }

    return frames;
}

std::vector<RuleHit> EvaluateRules(const std::vector<SourceFile>& sources)
{
    static const std::vector<Rule> rules = BuildRules();
    std::vector<RuleHit> hits;

    for (const Rule& rule : rules)
    {
        RuleHit hit;
        hit.rule = &rule;

        for (const SourceFile& source : sources)
        {
            for (size_t i = 0; i < source.lines.size(); ++i)
            {
                const std::string lowerLine = ToLower(source.lines[i]);
                int lineWeight = 0;

                for (const Pattern& pattern : rule.patterns)
                {
                    if (lowerLine.find(pattern.needle) != std::string::npos)
                        lineWeight += pattern.weight;
                }

                if (lineWeight <= 0)
                    continue;

                if (rule.id == "plugin")
                {
                    const bool hasFailureWord = lowerLine.find("fail") != std::string::npos ||
                        lowerLine.find("error") != std::string::npos ||
                        lowerLine.find("missing") != std::string::npos ||
                        lowerLine.find(".uplugin") != std::string::npos ||
                        lowerLine.find("module") != std::string::npos;
                    if (!hasFailureWord)
                        continue;
                }

                hit.score += std::min(lineWeight, 60);
                if (hit.evidence.size() < 8)
                    hit.evidence.push_back({ source.candidate.path, static_cast<int>(i + 1), Trim(source.lines[i]) });
            }
        }

        if (hit.score > 0)
            hits.push_back(std::move(hit));
    }

    std::sort(hits.begin(), hits.end(), [](const RuleHit& lhs, const RuleHit& rhs) {
        return lhs.score > rhs.score;
    });

    return hits;
}

std::string ConfidenceLabel(int score)
{
    if (score >= 70)
        return "high confidence";
    if (score >= 35)
        return "medium confidence";
    return "low confidence";
}

fs::path GetExecutableDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

    while (size == buffer.size())
    {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }

    if (size == 0)
        return fs::current_path();

    buffer.resize(size);
    return fs::path(buffer).parent_path();
}

std::optional<fs::path> GetLocalAppDataPath()
{
    wchar_t* value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, L"LOCALAPPDATA") != 0 || value == nullptr)
        return std::nullopt;

    fs::path path(value);
    std::free(value);
    return path;
}

void AppendEvidenceLine(std::ostringstream& out, const Evidence& evidence)
{
    out << "  " << PathString(evidence.path) << ":" << evidence.line << "\n";
    out << "    " << evidence.text << "\n";
}

std::string BuildReportText(const AnalysisResult& result)
{
    std::ostringstream out;
    out << "Unreal Crash Inspector Report\n";
    out << "Generated: " << NowForReport() << "\n\n";

    out << "Summary\n";
    if (!result.hasCrash)
    {
        out << "  No fatal Unreal crash marker was found in the scanned files.\n";
        out << "  Try passing --path to the game folder, Saved folder, latest .log, or latest Saved\\Crashes folder.\n\n";
    }
    else if (!result.ruleHits.empty())
    {
        const RuleHit& top = result.ruleHits.front();
        out << "  Likely cause: " << top.rule->title << " (" << ConfidenceLabel(top.score) << ")\n";
        out << "  " << top.rule->plainEnglish << "\n\n";
    }
    else
    {
        out << "  A fatal crash marker was found, but the category is unknown.\n";
        out << "  The primary error line and nearby log context below are still the useful part.\n\n";
    }

    if (!result.markers.empty())
    {
        out << "Primary Error Clues\n";
        for (size_t i = 0; i < std::min<size_t>(result.markers.size(), 8); ++i)
            AppendEvidenceLine(out, result.markers[i]);
        out << "\n";
    }

    if (!result.ruleHits.empty())
    {
        out << "Evidence\n";
        const RuleHit& top = result.ruleHits.front();
        for (const Evidence& evidence : top.evidence)
            AppendEvidenceLine(out, evidence);
        out << "\n";

        out << "Recommended Next Checks\n";
        for (size_t i = 0; i < top.rule->nextSteps.size(); ++i)
            out << "  " << (i + 1) << ". " << top.rule->nextSteps[i] << "\n";
        out << "\n";
    }

    if (!result.callstack.empty())
    {
        out << "Call Stack Highlights\n";
        size_t written = 0;
        for (const std::string& frame : result.callstack)
        {
            if (written >= 12)
                break;
            out << "  " << frame << "\n";
            ++written;
        }

        for (const std::string& frame : result.callstack)
        {
            if (!IsSystemStackFrame(frame))
            {
                out << "\nFirst non-system frame: " << frame << "\n";
                break;
            }
        }
        out << "\n";
    }

    if (!result.logContext.empty())
    {
        out << "Important Log Context\n";
        out << "  This is the area around the best fatal marker. Lines just above the marker often explain what Unreal was doing.\n\n";

        fs::path currentPath;
        for (const Evidence& context : result.logContext)
        {
            if (context.path != currentPath)
            {
                currentPath = context.path;
                out << "  File: " << PathString(currentPath) << "\n";
            }
            out << "  " << std::setw(6) << context.line << " | " << context.text << "\n";
        }
        out << "\n";
    }

    out << "Files Analyzed\n";
    for (size_t i = 0; i < std::min<size_t>(result.sources.size(), 20); ++i)
    {
        const SourceFile& source = result.sources[i];
        out << "  " << PathString(source.candidate.path)
            << " | modified " << FileTimeToString(source.candidate.modified)
            << " | score " << source.score << "\n";
    }
    out << "\n";

    if (!result.warnings.empty())
    {
        out << "Warnings\n";
        for (const std::string& warning : result.warnings)
            out << "  " << warning << "\n";
        out << "\n";
    }

    return out.str();
}

fs::path WriteReport(std::string text)
{
    fs::path reportDir = GetExecutableDirectory() / "CrashReports";
    std::error_code ec;
    fs::create_directories(reportDir, ec);
    if (ec)
    {
        reportDir = fs::current_path() / "CrashReports";
        fs::create_directories(reportDir, ec);
    }

    fs::path reportPath = reportDir / ("UnrealCrashReport-" + NowForFileName() + ".txt");
    std::ofstream file(reportPath, std::ios::binary);
    file << text;
    return reportPath;
}

AnalysisResult AnalyzeOnce(const Options& options)
{
    AnalysisResult result;
    std::vector<CandidateFile> candidates = DiscoverCandidates(options);
    result.sources = ReadSources(candidates, result.warnings);

    if (result.sources.empty())
    {
        result.warnings.push_back("No Unreal log or crash context files were found.");
        return result;
    }

    result.markers = GatherMarkers(result.sources);
    result.hasCrash = !result.markers.empty();
    result.logContext = BuildContext(result.sources, result.markers, options.contextBefore, options.contextAfter);
    result.callstack = ExtractCallstack(result.sources);
    result.ruleHits = EvaluateRules(result.sources);

    if (!options.noReport)
    {
        const std::string reportText = BuildReportText(result);
        result.reportPath = WriteReport(reportText);
    }

    return result;
}

void PrintSummary(const AnalysisResult& result)
{
    std::cout << "\nUnreal Crash Inspector\n";
    std::cout << "======================\n";

    if (result.sources.empty())
    {
        std::cout << "No Unreal log or crash files were found.\n";
    }
    else if (!result.hasCrash)
    {
        std::cout << "No fatal crash marker found in " << result.sources.size() << " file(s).\n";
        std::cout << "Newest analyzed file: " << PathString(result.sources.front().candidate.path) << "\n";
    }
    else if (!result.ruleHits.empty())
    {
        const RuleHit& top = result.ruleHits.front();
        std::cout << "Likely cause: " << top.rule->title << " (" << ConfidenceLabel(top.score) << ")\n";
        std::cout << Shorten(top.rule->plainEnglish, 220) << "\n";
    }
    else
    {
        std::cout << "Crash found, but the category is unknown.\n";
    }

    if (!result.markers.empty())
    {
        std::cout << "\nBest fatal clue:\n";
        std::cout << "  " << PathString(result.markers.front().path) << ":" << result.markers.front().line << "\n";
        std::cout << "  " << Shorten(result.markers.front().text, 220) << "\n";
    }

    if (!result.ruleHits.empty())
    {
        std::cout << "\nNext checks:\n";
        const RuleHit& top = result.ruleHits.front();
        for (size_t i = 0; i < std::min<size_t>(top.rule->nextSteps.size(), 3); ++i)
            std::cout << "  " << (i + 1) << ". " << top.rule->nextSteps[i] << "\n";
    }

    if (!result.callstack.empty())
    {
        for (const std::string& frame : result.callstack)
        {
            if (!IsSystemStackFrame(frame))
            {
                std::cout << "\nTop useful stack frame:\n  " << Shorten(frame, 220) << "\n";
                break;
            }
        }
    }

    if (!result.reportPath.empty())
        std::cout << "\nFull report written to:\n  " << PathString(result.reportPath) << "\n";

    if (!result.warnings.empty())
    {
        std::cout << "\nWarnings:\n";
        for (size_t i = 0; i < std::min<size_t>(result.warnings.size(), 5); ++i)
            std::cout << "  " << result.warnings[i] << "\n";
    }
}

void PrintHelp()
{
    std::cout <<
        "UnrealCrashInspector - external Unreal fatal error analyzer\n\n"
        "Usage:\n"
        "  UnrealCrashInspector.exe\n"
        "  UnrealCrashInspector.exe --game MyGame\n"
        "  UnrealCrashInspector.exe --path \"C:\\Games\\MyGame\"\n"
        "  UnrealCrashInspector.exe --path \"%LOCALAPPDATA%\\MyGame\\Saved\\Crashes\"\n"
        "  UnrealCrashInspector.exe --watch --game MyGame\n\n"
        "Options:\n"
        "  -p, --path <path>       Scan a game folder, Saved folder, crash folder, .log, or CrashContext file.\n"
        "  -g, --game <name>       Filter automatic %LOCALAPPDATA% scanning to paths containing this game name.\n"
        "  -w, --watch             Keep scanning and report when a new crash appears.\n"
        "      --open-report       Open the written text report in the default editor.\n"
        "      --no-report         Print only to console without writing a report file.\n"
        "      --context <lines>   Number of lines to include before the fatal marker. Default is 50.\n"
        "  -h, --help              Show this help.\n\n"
        "Tip: You can drag a .log file or a Saved\\Crashes folder onto the executable.\n";
}

bool NeedsValue(int index, int argc, const std::wstring& option)
{
    if (index + 1 < argc)
        return false;

    std::wcerr << L"Missing value after " << option << L"\n";
    return true;
}

Options ParseOptions(int argc, wchar_t** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help" || arg == L"/?")
        {
            options.help = true;
        }
        else if (arg == L"-w" || arg == L"--watch")
        {
            options.watch = true;
        }
        else if (arg == L"--no-report")
        {
            options.noReport = true;
        }
        else if (arg == L"--open-report")
        {
            options.openReport = true;
        }
        else if (arg == L"-p" || arg == L"--path")
        {
            if (NeedsValue(i, argc, arg))
                break;
            options.roots.emplace_back(argv[++i]);
        }
        else if (arg == L"-g" || arg == L"--game")
        {
            if (NeedsValue(i, argc, arg))
                break;
            options.gameFilter = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--context")
        {
            if (NeedsValue(i, argc, arg))
                break;
            options.contextBefore = std::max(5, _wtoi(argv[++i]));
        }
        else if (!arg.empty() && arg[0] == L'-')
        {
            std::wcerr << L"Unknown option: " << arg << L"\n";
            options.help = true;
        }
        else
        {
            options.roots.emplace_back(arg);
        }
    }

    options.contextAfter = std::max(10, options.contextBefore / 2);
    return options;
}

std::wstring PathToWide(const fs::path& path)
{
    return path.wstring();
}

void OpenReportIfRequested(const Options& options, const fs::path& reportPath)
{
    if (!options.openReport || reportPath.empty())
        return;

    const std::wstring widePath = PathToWide(reportPath);
    ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring LatestSignature(const AnalysisResult& result)
{
    if (!result.markers.empty())
        return result.markers.front().path.wstring() + L"|" + std::to_wstring(result.markers.front().line);
    if (!result.sources.empty())
        return result.sources.front().candidate.path.wstring() + L"|" + std::to_wstring(result.sources.front().score);
    return {};
}

int Run(int argc, wchar_t** argv)
{
    const Options options = ParseOptions(argc, argv);
    if (options.help)
    {
        PrintHelp();
        return 0;
    }

    if (!options.watch)
    {
        AnalysisResult result = AnalyzeOnce(options);
        PrintSummary(result);
        OpenReportIfRequested(options, result.reportPath);
        return result.hasCrash ? 0 : 1;
    }

    std::cout << "Watching Unreal logs and crash folders. Press Ctrl+C to stop.\n";
    std::wstring lastSignature;

    for (;;)
    {
        AnalysisResult result = AnalyzeOnce(options);
        const std::wstring signature = LatestSignature(result);
        if (!signature.empty() && signature != lastSignature)
        {
            lastSignature = signature;
            PrintSummary(result);
            OpenReportIfRequested(options, result.reportPath);
        }

        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "UnrealCrashInspector failed: " << exception.what() << "\n";
        return 2;
    }
}
