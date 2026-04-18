// 2>/dev/null; d=$(dirname "$(readlink -f "$0")"); o="$d/build/.bootstrap"; x="$o.d"; mkdir -p "$d/build"; s=0; if [ -f "$o" ] && [ -f "$x" ]; then s=1; while IFS= read -r f; do [ -z "$f" ] && continue; [ "$f" -nt "$o" ] && s=0 && break; done < <(sed -e 's/\\$//' "$x" | tr ' ' '\n' | sed -n '/:/d; /^$/d; p'); else s=0; fi; [ "$s" = 1 ] || "${CXX:-clang++}" -std=c++26 -I"$d" -MD -MF "$x" -x c++ "$0" -o "$o" "${CXXFLAGS_BUILD:-}" || exit $?; exec "$o" "$@"
// ═══════════════════════════════════════════════════════════════════════════
//  build.cxx — C++26 consteval build system, project definition.
//
//  Run:  chmod +x build.cxx  (once)
//        ./build.cxx         (every time)
//
//  Line 1 is both a `//` C++ comment and a shell command. The shell
//  compiles this TU once with -std=c++26, caches the result in
//  build/.bootstrap (mtime-based), then execs it. Warm runs are ~40 ms.
//  Swap compilers with CXX=g++ ./build.cxx.
// ═══════════════════════════════════════════════════════════════════════════

#include "build_cxx/build.hxx"

#include "src/examples/01_hello/build.cxx"
#include "src/examples/02_static_lib/build.cxx"
#include "src/examples/03_shared_lib/build.cxx"
#include "src/examples/04_unity_build/build.cxx"
#include "src/examples/05_glob_sources/build.cxx"
#include "src/examples/06_nested_subdirs/build.cxx"
#include "src/examples/07_custom_flags/build.cxx"
#include "src/examples/08_sdl3/build.cxx"
#include "src/examples/09_generate_and_postbuild/build.cxx"
#include "src/examples/10_app_with_tests/build.cxx"
#include "src/examples/11_runnables/build.cxx"

// ═══════════════════════════════════════════════════════════════════════════
//  YOUR PROJECT
// ═══════════════════════════════════════════════════════════════════════════

constexpr auto configure = []() consteval -> build::project {
    using namespace build;
    using enum standard;
    using enum compiler_id;

    project p("cppbuild", { .major = 1 });
    p.cc(clang).out("build").std(cpp23).release().werror();

    // Subdir modules. Each ex_* is a plain consteval function taking
    // `project&` and adding its targets / generated files / runnables.
    // `p.add(fn)` is the unified compose verb (equivalent to calling
    // the function directly — use whichever reads better).
    p.add(ex_01_hello);
    p.add(ex_02_static_lib);
    p.add(ex_03_shared_lib);
    p.add(ex_04_unity_build);
    p.add(ex_05_glob_sources);
    p.add(ex_06_nested);
    p.add(ex_07_custom_flags);
    p.add(ex_08_sdl3);
    p.add(ex_09_generate_and_postbuild);
    p.add(ex_10_app_with_tests);
    p.add(ex_11_runnables);

    // Project-level lifecycle hooks: p.pre / p.post run once per
    // invocation (build or runnable), bracketing real work. Unlike
    // target .before/.after, these fire every time regardless of
    // cache state — use them for build-wide banners, timing, audit.
    p.pre(+[](build::project_ctx c) -> std::string {
        return std::format("echo '>>> {} v{} starting'", c.project, c.version);
    });
    p.post(+[](build::project_ctx c) -> std::string {
        return std::format("echo '<<< {} build finished — {}'",
                           c.project, c.build_dir);
    });

    // Self-test for the build system itself: invokes the compiled
    // driver (build/.bootstrap) as a subprocess and checks user-visible
    // invariants. Running `./build.cxx --run selftest` builds the
    // driver, then runs the script; any failure exits non-zero.
    p.runnable("selftest")
        .cmd(+[](build::ctx c) -> std::string {
            return std::format(
                "bash {0}/src/examples/11_runnables/selftest.sh {0} {1}",
                c.root, c.build_dir);
        });

    return p;
};

build::run<configure> app;

// main noexcept: std::format / std::print can throw format_error on bad
// format strings; our format strings are all static so it won't happen,
// but tidy can't prove it. Any escape degenerates to a clean exit 1
// with a message rather than std::terminate.
int main(int argc, char** argv) noexcept
{
    try {
        return app(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "build.cxx: fatal: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "build.cxx: fatal: unknown exception\n");
        return 1;
    }
}
