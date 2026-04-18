// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/driver.hxx — CLI entry point.
//
//  User writes:
//
//      constexpr auto configure = []() consteval -> build::project {
//          build::project p("myapp", { .major = 1 });
//          p.exe("myapp", { "main.cpp" });
//          return p;
//      };
//      build::run<configure> app;
//      int main(int argc, char** argv) { return app(argc, argv); }
//
//  `run<configure>` evaluates the project at class-template-instantiation
//  time, fires validation static_asserts, and stores a ready-to-use
//  executor. main(argc, argv) drives the executor via operator().
//
//  Subcommands (flags, any order):
//
//      (none)               build everything
//      --help, -h           print usage
//      --list               list targets and runnables
//      --target <name>      build target + its transitive deps only
//      --run <name>         build runnable's deps, then run its commands
//      --dry-run            print the plan, don't execute
//      --clean              rm -rf build dir (+ compile_commands.json)
//      --clean-artifacts    remove build outputs, keep _deps/ + .bootstrap
//      --why                print the reason each step rebuilt
//      -j <N>               cap per-wave parallel compiles at N
//      --emit ninja         write <build_dir>/build.ninja (override with =path)
//      --emit compile-db    run prepare() only (writes compile DB)
//      --verbose            dep-walk + substitution trace (CPPBUILD_VERBOSE=1)
//      --quiet              equivalent to CPPBUILD_QUIET=1
//
//  All modes except --help/--list/--clean run prepare() first (probe,
//  generate files, fetch, expand, write compile_commands.json). Only
//  (none) and --target actually execute the plan.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "emit_ninja.hxx"
#include "executor.hxx"
#include "plan.hxx"
#include "project.hxx"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <print>
#include <string_view>
#include <system_error>

namespace build {

// Pin the driver's cwd to the project root (directory containing the
// root build.cxx) so user runnable commands see a stable working
// directory regardless of where the user invoked us from.
//
// The shebang execs build/.bootstrap with an absolute path, so argv[0]
// is reliably resolvable. We canonicalize argv[0] -> build/.bootstrap,
// take parent_path().parent_path() -> project root, and chdir there.
//
// Silent best-effort: if we can't derive a path we leave cwd untouched
// (users can still pass --target from anywhere, just with the usual
// cwd-relative semantics).
inline void pin_cwd_to_project_root(const char* argv0) noexcept
{
    namespace fs = std::filesystem;
    if (argv0 == nullptr || argv0[0] == '\0') return;
    std::error_code ec;
    auto exe = fs::canonical(argv0, ec);
    if (ec || !exe.has_parent_path()) return;
    auto root = exe.parent_path().parent_path();  // build/ -> project root
    if (root.empty()) return;
    fs::current_path(root, ec);
}

inline void print_usage(const char* argv0)
{
    std::println(stderr,
        "usage: {} [flags]\n"
        "\n"
        "  (none)              build everything\n"
        "  --list              list targets and runnables, then exit\n"
        "  --target <name>     build target + its transitive deps only\n"
        "  --run <name>        build runnable's deps, then run its commands\n"
        "  --dry-run           print the plan, don't execute\n"
        "  --clean             remove build dir and compile_commands.json\n"
        "  --clean-artifacts   remove outputs, keep _deps/ and .bootstrap\n"
        "  --why               print rebuild reason per step (CPPBUILD_WHY=1)\n"
        "  -j <N>              cap per-wave parallel compiles at N (default: unbounded)\n"
        "  --emit ninja[=path] write build.ninja (default: <build_dir>/build.ninja)\n"
        "  --emit compile-db   write compile_commands.json, no execution\n"
        "  --verbose           dep-walk + substitution trace (CPPBUILD_VERBOSE=1)\n"
        "  --quiet             suppress per-step command echo (also CPPBUILD_QUIET=1)\n"
        "  --help, -h          this message",
        argv0);
}

template <auto const& Configure> struct run final
{
    static constexpr auto project_ = Configure();
    static constexpr auto plan_    = build::make_plan(project_);

    static_assert(!plan_.steps.is_empty() || !plan_.deferred.is_empty(),
                  "build plan has zero steps — project declares no targets");
    static_assert(!plan_.db_entries.is_empty() || !plan_.deferred.is_empty(),
                  "compile database is empty — project declares no targets");

    using executor_t = build::executor<plan_, project_>;

    // main(argc, argv) { return app(argc, argv); }
    auto operator()(int argc, char** argv) const -> int
    {
        const char* argv0      = argc > 0 ? argv[0] : "build.cxx";
        pin_cwd_to_project_root(argv0);
        std::string_view only_target;
        std::string_view run_name;
        std::size_t      jobs        = 0;
        bool             want_list            = false;
        bool             want_dry             = false;
        bool             want_clean           = false;
        bool             want_clean_artifacts = false;
        bool             want_help            = false;
        std::string_view want_emit;

        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a == "--help" || a == "-h") {
                want_help = true;
            } else if (a == "--list") {
                want_list = true;
            } else if (a == "--dry-run") {
                want_dry = true;
            } else if (a == "--clean") {
                want_clean = true;
            } else if (a == "--clean-artifacts") {
                want_clean_artifacts = true;
            } else if (a == "--quiet") {
                portable::set_env("CPPBUILD_QUIET", "1");
            } else if (a == "--why") {
                portable::set_env("CPPBUILD_WHY", "1");
            } else if (a == "--verbose") {
                portable::set_env("CPPBUILD_VERBOSE", "1");
            } else if (a == "--target") {
                if (i + 1 >= argc) {
                    std::println(stderr, "--target needs a name");
                    return 2;
                }
                only_target = argv[++i];
            } else if (a == "--run") {
                if (i + 1 >= argc) {
                    std::println(stderr, "--run needs a name");
                    return 2;
                }
                run_name = argv[++i];
            } else if (a == "-j") {
                if (i + 1 >= argc) {
                    std::println(stderr, "-j needs an integer");
                    return 2;
                }
                std::string_view n = argv[++i];
                auto [_, ec] = std::from_chars(
                    n.data(), n.data() + n.size(), jobs);
                if (ec != std::errc{}) {
                    std::println(stderr, "-j: bad integer '{}'", n);
                    return 2;
                }
            } else if (a == "--emit") {
                if (i + 1 >= argc) {
                    std::println(stderr,
                        "--emit needs a format (ninja | compile-db)");
                    return 2;
                }
                want_emit = argv[++i];
            } else {
                std::println(stderr, "unknown flag: {}", a);
                print_usage(argv0);
                return 2;
            }
        }

        if (want_help) { print_usage(argv0); return 0; }

        if (want_list) {
            executor_t::print_targets();
            return 0;
        }

        if (want_clean) {
            executor_t::clean();
            return 0;
        }

        if (want_clean_artifacts) {
            executor_t::clean_artifacts();
            return 0;
        }

        if (!only_target.empty() && !project_.find_target(only_target)) {
            std::println(stderr, "unknown target: '{}'", only_target);
            executor_t::print_targets();
            return 2;
        }

        if (!run_name.empty() && !project_.find_runnable(run_name)) {
            std::println(stderr, "unknown runnable: '{}'", run_name);
            executor_t::print_targets();
            return 2;
        }

        auto ses = executor_t::prepare();

        if (want_emit.starts_with("ninja")) {
            // Default: <build_dir>/build.ninja — keeps the source tree
            // tidy. Override with `--emit ninja=./custom.path`.
            std::string path = std::string(project_.build_dir_) + "/build.ninja";
            if (want_emit.size() > 5) {
                if (want_emit[5] != '=') {
                    std::println(stderr,
                        "--emit ninja: use --emit ninja=<path>, got '{}'",
                        want_emit);
                    return 2;
                }
                path = std::string(want_emit.substr(6));
            }
            auto body = emit_ninja(ses, project_, project_.build_dir_);
            std::FILE* f = std::fopen(path.c_str(), "w");
            if (!f) {
                std::println(stderr, "cannot write '{}'", path);
                return 1;
            }
            std::fwrite(body.data(), 1, body.size(), f);
            std::fclose(f);
            std::println(stderr, "\n● wrote {} ({} steps)",
                         path, ses.flat_steps.size());
            return 0;
        }
        if (want_emit == "compile-db") {
            // prepare() already wrote it.
            return 0;
        }
        if (!want_emit.empty()) {
            std::println(stderr,
                "--emit: unknown format '{}' (try ninja | compile-db)",
                want_emit);
            return 2;
        }

        if (want_dry) {
            executor_t::print_plan(ses);
            return 0;
        }

        // Project-level pre/post hooks bracket real work (build or
        // runnable). Pure-info commands above (--help, --list, --emit,
        // --dry-run, --clean) do not trigger them.
        if (!executor_t::run_project_hooks(project_.pre_hooks_, "pre"))
            return 1;

        int rc;
        if (!run_name.empty()) {
            const auto* r = project_.find_runnable(run_name);
            rc = executor_t::run_runnable(ses, *r, jobs);
        } else {
            rc = executor_t::run_filtered(ses, only_target, jobs);
        }

        if (rc == 0) {
            if (!executor_t::run_project_hooks(project_.post_hooks_, "post"))
                return 1;
        }
        return rc;
    }
};

} // namespace build
