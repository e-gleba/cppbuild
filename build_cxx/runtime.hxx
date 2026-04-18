// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/runtime.hxx — runtime helpers: ansi colours, JSON escaping,
//  popen piping, tokenizer, depfile parsing, compiler introspection.
//
//  Introspection relies on flags that both clang and gcc understand
//  (-dumpmachine, -print-resource-dir on clang, -print-search-dirs, -E -v
//  for system-include discovery). We never hardcode paths; if a flag is
//  unsupported the result is empty and the caller degrades gracefully.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "portable.hxx"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace build {

// Join a range of string-like items with a separator. String-building
// in a dozen call sites collapses to one line.
template <std::ranges::input_range R>
[[nodiscard]] inline auto join(R&& items, std::string_view sep) -> std::string
{
    std::string out;
    bool first = true;
    for (const auto& x : items) {
        if (!first) out.append(sep);
        out.append(std::string_view(x));
        first = false;
    }
    return out;
}

namespace ansi {
constexpr auto reset  = "\033[0m";
constexpr auto bold   = "\033[1m";
constexpr auto red    = "\033[1;31m";
constexpr auto green  = "\033[1;32m";
constexpr auto yellow = "\033[0;33m";
constexpr auto cyan   = "\033[1;36m";
constexpr auto grey   = "\033[0;37m";
} // namespace ansi

[[nodiscard]] inline auto json_escape(std::string_view sv) -> std::string
{
    std::string o;
    o.reserve(sv.size() + 16);
    for (char c : sv) {
        if (c == '\\') {
            o += "\\\\";
        } else if (c == '"') {
            o += "\\\"";
        } else {
            o += c;
        }
    }
    return o;
}

[[nodiscard]] inline auto pipe_cmd(const std::string& cmd) -> std::string
{
    std::string o;
    if (auto* p = portable::open_pipe(cmd.c_str())) {
        char b[512];
        while (std::fgets(b, sizeof b, p)) {
            o += b;
        }
        portable::close_pipe(p);
    }
    while (!o.empty() && o.back() == '\n') {
        o.pop_back();
    }
    return o;
}

[[nodiscard]] inline auto tokenize(std::string_view sv)
    -> std::vector<std::string>
{
    std::vector<std::string> t;
    std::size_t              p = 0;
    while (p < sv.size()) {
        while (p < sv.size() && sv[p] == ' ') {
            ++p;
        }
        auto e = sv.find(' ', p);
        if (e == std::string_view::npos) {
            e = sv.size();
        }
        if (p < e) {
            t.emplace_back(sv.substr(p, e - p));
        }
        p = e;
    }
    return t;
}

// Parse `#include <...> search starts here:` section of clang/gcc -v.
[[nodiscard]] inline auto query_system_includes(const std::string& compiler)
    -> std::vector<std::string>
{
    auto raw = pipe_cmd(compiler + " -E -x c++ /dev/null -v 2>&1");
    std::vector<std::string> paths;

    constexpr std::string_view begin_tag = "#include <...> search starts here:";
    constexpr std::string_view end_tag   = "End of search list.";

    auto s = raw.find(begin_tag);
    auto e = raw.find(end_tag);
    if (s == std::string::npos || e == std::string::npos) {
        return paths;
    }
    s += begin_tag.size();

    std::string_view section(raw.data() + s, e - s);
    std::size_t      pos = 0;
    while (pos < section.size()) {
        while (pos < section.size() &&
               (section[pos] == ' ' || section[pos] == '\t')) {
            ++pos;
        }
        auto nl = section.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = section.size();
        }
        auto line = section.substr(pos, nl - pos);
        if (auto fw = line.find(" (framework directory)");
            fw != std::string_view::npos) {
            line = line.substr(0, fw);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                 line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            paths.emplace_back(line);
        }
        pos = nl + 1;
    }
    return paths;
}

// Full self-description. Everything optional; empty on unsupported flags.
struct compiler_probe final
{
    std::string              absolute_path; // `which <compiler>`
    std::string              version;       // first line of --version
    std::string              resource_dir;  // clang: -print-resource-dir
    std::string              target_triple; // -dumpmachine (clang & gcc)
    std::vector<std::string> system_includes;
};

[[nodiscard]] inline auto probe(const std::string& compiler) -> compiler_probe
{
    compiler_probe pr;
    pr.absolute_path = pipe_cmd("which " + compiler);
    if (pr.absolute_path.empty()) {
        pr.absolute_path = compiler;
    }
    pr.resource_dir = pipe_cmd(compiler + " -print-resource-dir 2>/dev/null");
    pr.target_triple = pipe_cmd(compiler + " -dumpmachine 2>/dev/null");
    // --version prints a banner; first line is consistent across vendors
    // and contains the version number. Take just that line.
    auto raw_ver = pipe_cmd(compiler + " --version 2>/dev/null");
    if (auto nl = raw_ver.find('\n'); nl != std::string::npos) {
        raw_ver.resize(nl);
    }
    pr.version         = std::move(raw_ver);
    pr.system_includes = query_system_includes(compiler);
    return pr;
}

inline void write_clangd_config(std::string_view build_dir)
{
    namespace fs   = std::filesystem;
    const auto root = fs::current_path().string();

    auto* f = std::fopen(".clangd", "w");
    if (!f) {
        return;
    }
    // Everything clangd needs:
    //   - CompilationDatabase → per-TU flags for tracked targets,
    //   - Add: -I<project_root> so subdir build.cxx files (which
    //     #include "build_cxx/build.hxx") parse standalone without
    //     relative-path gymnastics,
    //   - Add: -std=c++26 so the build.cxx driver file itself parses.
    std::println(f,
                 "# Auto-generated by build.cxx — do not hand-edit\n"
                 "CompileFlags:\n"
                 "  CompilationDatabase: {:.{}}\n"
                 "  Add:\n"
                 "    - -I{}\n"
                 "    - -std=c++26\n\n"
                 "Diagnostics:\n"
                 "  UnusedIncludes: Strict",
                 build_dir.data(),
                 static_cast<int>(build_dir.size()),
                 root);
    std::fclose(f);
}

// Content-aware file write: only touch the file on disk when bytes
// differ. Keeps mtime stable across builds so downstream incremental
// checks stay warm. Returns true when the file was actually written.
[[nodiscard]] inline auto write_if_different(
    const std::filesystem::path& path,
    std::string_view             content) -> bool
{
    namespace fs = std::filesystem;
    if (fs::exists(path)) {
        std::ifstream     in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        if (ss.str() == content) {
            return false;
        }
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

// Parse a Make-style depfile emitted by clang/gcc (-MD -MF). Format is:
//   <output>: <dep1> <dep2> \\
//     <dep3> ...
// Backslash-newline line continuations, space-separated deps. The output
// name itself is the first colon-terminated token and is skipped.
[[nodiscard]] inline auto parse_depfile(const std::filesystem::path& path)
    -> std::vector<std::string>
{
    std::vector<std::string> deps;
    std::ifstream            in(path);
    if (!in) {
        return deps;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();

    // Strip line continuations.
    std::string flat;
    flat.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size() && body[i + 1] == '\n') {
            ++i;
            flat += ' ';
        } else if (body[i] == '\n') {
            flat += ' ';
        } else {
            flat += body[i];
        }
    }

    auto colon = flat.find(':');
    if (colon == std::string::npos) {
        return deps;
    }
    std::string_view rhs(flat.data() + colon + 1, flat.size() - colon - 1);
    std::size_t      p = 0;
    while (p < rhs.size()) {
        while (p < rhs.size() && (rhs[p] == ' ' || rhs[p] == '\t')) {
            ++p;
        }
        auto e = p;
        while (e < rhs.size() && rhs[e] != ' ' && rhs[e] != '\t') {
            if (rhs[e] == '\\' && e + 1 < rhs.size()) {
                ++e; // escaped char (e.g. "\ " for spaces in paths)
            }
            ++e;
        }
        if (e > p) {
            deps.emplace_back(rhs.substr(p, e - p));
        }
        p = e;
    }
    return deps;
}

// Staleness with a human-readable reason. Empty reason = fresh.
// Filled on the *stale* path only; the fresh path allocates nothing.
struct staleness final
{
    bool        fresh{ false };
    std::string reason{};

    [[nodiscard]] static auto ok() -> staleness
    {
        return { .fresh = true };
    }
    [[nodiscard]] static auto stale(std::string why) -> staleness
    {
        return { .fresh = false, .reason = std::move(why) };
    }
    explicit operator bool() const noexcept { return fresh; }
};

// Fresh iff `out` exists and is newer than every path in `inputs`. Missing
// inputs are ignored (depfile may reference a since-removed header);
// missing `out` => stale. Reason on stale: "output missing" or
// "input newer: <path>".
[[nodiscard]] inline auto check_mtime(const std::filesystem::path&    out,
                                      const std::vector<std::string>& inputs)
    -> staleness
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(out, ec)) {
        return staleness::stale("output missing");
    }
    const auto out_time = fs::last_write_time(out, ec);
    if (ec) {
        return staleness::stale("cannot stat output");
    }
    for (const auto& in : inputs) {
        if (!fs::exists(in, ec)) {
            continue;
        }
        const auto in_time = fs::last_write_time(in, ec);
        if (ec) {
            continue;
        }
        if (in_time > out_time) {
            return staleness::stale("input newer: " + in);
        }
    }
    return staleness::ok();
}

// 64-bit FNV-1a. Good enough for command-fingerprint comparison; we're
// not authenticating, just detecting "did the flags change?".
[[nodiscard]] inline auto fnv1a64(std::string_view s) -> std::uint64_t
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Path to the stamp file for an output. Lives under
// <build_dir>/.cache/ so `--clean` wipes it with the rest.
[[nodiscard]] inline auto stamp_path_of(const std::filesystem::path& out,
                                        std::string_view build_dir)
    -> std::filesystem::path
{
    namespace fs = std::filesystem;
    auto name = out.string();
    for (auto& c : name) if (c == '/' || c == '\\') c = '_';
    return fs::path(std::string(build_dir)) / ".cache" / (name + ".stamp");
}

[[nodiscard]] inline auto read_stamp(const std::filesystem::path& p) -> std::string
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void write_stamp(const std::filesystem::path& p, std::string_view s)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// mtime check + command-fingerprint check. Fresh only when both agree.
// Closes the CMake-classic "changed -O2 to -O0 but nothing rebuilt"
// hole without full content hashing of inputs.
[[nodiscard]] inline auto check_staleness(
    const std::filesystem::path&    out,
    const std::vector<std::string>& inputs,
    std::string_view                command,
    std::string_view                build_dir) -> staleness
{
    if (auto s = check_mtime(out, inputs); !s) {
        return s;
    }
    const auto stamp = stamp_path_of(out, build_dir);
    const auto saved = read_stamp(stamp);
    const auto cur   = std::to_string(fnv1a64(command));
    if (saved != cur) {
        return staleness::stale("command hash changed");
    }
    return staleness::ok();
}

inline void update_stamp(const std::filesystem::path& out,
                         std::string_view             command,
                         std::string_view             build_dir)
{
    write_stamp(stamp_path_of(out, build_dir),
                std::to_string(fnv1a64(command)));
}

} // namespace build
