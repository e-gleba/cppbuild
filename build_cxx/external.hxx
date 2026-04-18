// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/external.hxx — runtime execution of fetches and external
//  projects.
//
//  fetch_git:
//    build/_deps/<name>/src/.git exists → nothing to do (idempotent).
//    Otherwise: git clone --depth 1 --branch <rev> <url> <dir>, falling
//    back to a full clone + git checkout <rev> when --branch won't take
//    a commit sha. All progress goes to the executor's normal log.
//
//  external_project:
//    Runs configure_cmd then build_cmd with these substitutions:
//      @SRC@   → build/_deps/<name>/src
//      @BUILD@ → build/_deps/<name>/build
//      @ROOT@  → project root (std::filesystem::current_path at exec)
//    env("KEY=VAL", ...) is prepended to the shell invocation of both
//    phases. define("KEY=VAL", ...) is appended as `-D<entry>` to the
//    configure phase only. Each phase is skipped when its marker file
//    (.configured / .built) exists — a pragmatic, dependency-free
//    incremental story. To force a rebuild, rm -rf build/_deps.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "errors.hxx"
#include "project.hxx"
#include "runtime.hxx"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

namespace build {

namespace ext_detail {

inline auto replace_all(std::string s,
                        std::string_view from,
                        std::string_view to) -> std::string
{
    if (from.empty()) {
        return s;
    }
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

inline auto substitute(std::string_view tmpl,
                       std::string_view src,
                       std::string_view bld,
                       std::string_view root) -> std::string
{
    auto r = replace_all(std::string(tmpl), "@SRC@", src);
    r      = replace_all(r, "@BUILD@", bld);
    r      = replace_all(r, "@ROOT@", root);
    return r;
}

// Build the final shell invocation for an external phase.
//   [env ...] <substituted cmd> [ -Ddef ... ]
// defines are only appended to the configure phase (caller controls that
// by passing an empty span for the build phase).
inline auto compose_cmd(const external_project_spec& e,
                        std::string_view             phase_cmd,
                        std::string_view             src,
                        std::string_view             bld,
                        std::string_view             root,
                        bool                         append_defines)
    -> std::string
{
    std::string out;
    for (const auto& kv : e.env_vars) {
        out.append(kv);
        out.push_back(' ');
    }
    out.append(substitute(phase_cmd, src, bld, root));
    if (append_defines) {
        for (const auto& kv : e.defines) {
            out.append(" -D");
            out.append(kv);
        }
    }
    return out;
}

} // namespace ext_detail

inline auto fetch_dir(std::string_view build_dir, std::string_view name)
    -> std::filesystem::path
{
    return std::filesystem::path(std::string(build_dir)) / "_deps" /
           std::string(name);
}

// Returns true when the checkout was newly populated (so downstream
// builds must run). False when already present.
inline auto run_fetch(const fetch_git_spec& f, std::string_view build_dir)
    -> bool
{
    namespace fs = std::filesystem;
    using namespace ansi;

    auto root = fetch_dir(build_dir, f.name);
    auto src  = root / "src";

    if (fs::exists(src / ".git")) {
        std::println(stderr,
                     "{}▸{} fetch {}{}{}  {}(cached){} {}",
                     cyan,
                     reset,
                     bold,
                     std::string_view(f.name),
                     reset,
                     grey,
                     reset,
                     src.string());
        return false;
    }

    fs::create_directories(root);
    std::string cmd = "git clone --depth 1 --branch " + std::string(f.rev) +
                      ' ' + std::string(f.url) + ' ' + src.string() +
                      " 2>&1";
    std::println(stderr,
                 "{}▸{} fetch {}{}{} @ {}",
                 cyan,
                 reset,
                 bold,
                 std::string_view(f.name),
                 reset,
                 std::string_view(f.rev));
    std::println(stderr, "  {}{}{}", yellow, cmd, reset);
    int rc = std::system(cmd.c_str());

    // --branch doesn't accept bare commit SHAs. Retry with full clone +
    // explicit checkout if the shallow form failed.
    if (rc != 0) {
        std::error_code ec;
        fs::remove_all(src, ec);
        std::string full =
            "git clone " + std::string(f.url) + ' ' + src.string() +
            " && (cd " + src.string() + " && git checkout " +
            std::string(f.rev) + ") 2>&1";
        std::println(stderr, "  {}{}{}", yellow, full, reset);
        rc = std::system(full.c_str());
    }

    if (rc != 0) {
        die("fetch_git failed", std::string(f.name));
    }
    std::println(stderr, "  {}✓{} {}", green, reset, src.string());
    return true;
}

inline auto run_external(const external_project_spec& e,
                         std::string_view             build_dir,
                         bool                         source_is_new) -> void
{
    namespace fs = std::filesystem;
    using namespace ansi;

    if (e.configure_cmd.empty() && e.build_cmd.empty()) {
        return; // fetch-only external, nothing to run
    }

    const auto root  = fetch_dir(build_dir, e.name);
    const auto src   = e.source_name.empty()
                           ? root / "src" // self-hosted external: no git fetch
                           : fetch_dir(build_dir, e.source_name) / "src";
    const auto bld   = root / "build";
    const auto cfg_m = bld / ".configured";
    const auto bld_m = bld / ".built";
    const auto out   = bld / std::string(e.produces_file);

    fs::create_directories(bld);

    if (!fs::exists(src)) {
        die("external_project source not found",
            std::string(e.name) + " → " + src.string());
    }

    auto src_sv  = src.string();
    auto bld_sv  = bld.string();
    auto root_sv = fs::current_path().string();

    // ── configure phase ─────────────────────────────────────────────
    //
    // Marker-based: if `.configured` exists and the source wasn't just
    // freshly cloned, we trust it. Changing the external's source tree
    // outside of a new fetch is rare; when you do, `rm -rf build/_deps`
    // to rebuild the deps cleanly.
    bool need_configure = source_is_new || !fs::exists(cfg_m);
    if (!e.configure_cmd.empty()) {
        if (need_configure) {
            auto cmd = ext_detail::compose_cmd(
                e, e.configure_cmd, src_sv, bld_sv, root_sv,
                /*append_defines=*/true);
            std::println(stderr,
                         "{}▸{} configure {}{}{}",
                         cyan,
                         reset,
                         bold,
                         std::string_view(e.name),
                         reset);
            std::println(stderr, "  {}{}{}", yellow, cmd, reset);
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                die("external_project configure failed", std::string(e.name));
            }
            std::ofstream(cfg_m).put('\n');
            std::println(stderr, "  {}✓{} configured", green, reset);
        } else {
            std::println(stderr,
                         "{}▸{} configure {}{}{}  {}(cached){}",
                         cyan,
                         reset,
                         bold,
                         std::string_view(e.name),
                         reset,
                         grey,
                         reset);
        }
    }

    // ── build phase ─────────────────────────────────────────────────
    bool need_build = need_configure || !fs::exists(bld_m) ||
                      (!e.produces_file.empty() && !fs::exists(out));
    if (!e.build_cmd.empty()) {
        if (need_build) {
            auto cmd = ext_detail::compose_cmd(
                e, e.build_cmd, src_sv, bld_sv, root_sv,
                /*append_defines=*/false);
            std::println(stderr,
                         "{}▸{} build {}{}{}",
                         cyan,
                         reset,
                         bold,
                         std::string_view(e.name),
                         reset);
            std::println(stderr, "  {}{}{}", yellow, cmd, reset);
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                die("external_project build failed", std::string(e.name));
            }
            std::ofstream(bld_m).put('\n');
            std::println(stderr,
                         "  {}✓{} {}",
                         green,
                         reset,
                         e.produces_file.empty()
                             ? std::string("built")
                             : out.string());
        } else {
            std::println(stderr,
                         "{}▸{} build {}{}{}  {}(cached){} {}",
                         cyan,
                         reset,
                         bold,
                         std::string_view(e.name),
                         reset,
                         grey,
                         reset,
                         out.string());
        }
    }
}

} // namespace build
