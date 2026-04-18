# build.cxx

<p align="center">
  <img src=".github/logo.png" alt="cxx-skeleton logo" width="200"/>
</p>

<p align="center">
  <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-23%2F26-00599C?style=for-the-badge&logo=cplusplus&logoColor=white&labelColor=1C1C1C" alt="C++ Standard"/></a>
  <a href="license"><img src="https://img.shields.io/badge/License-MIT-blue?style=for-the-badge&labelColor=1C1C1C" alt="License"/></a>
</p>

A C++ build system written in C++.
No CMake. No Meson. No Bazel. No external DSL. No new language to learn. Just C++26.

## The problem

Every C++ developer knows the pain. You want to write C++, but before
you write a single line of actual code, you need to learn CMake — a
separate language with its own syntax, semantics, gotchas, Stack
Overflow rabbit holes. Then Conan or vcpkg on top. Then Ninja. Then you
wire it all together and pray. The build system is the only part of a
C++ project that isn't C++.

## The idea

What if it was?

`build.cxx` is a single-file build system where everything — project
name, version, targets, sources, include directories, flags,
definitions, dependencies — is plain C++26 via `consteval`. The
compiler validates your entire build at compile time. Typo a target
name, reference a nonexistent dependency, forget sources: compile
error, not a 20-minute CI pipeline failure.

The engine (primitives, validation, command generation, plan
execution) lives above `main()` and is universal — you don't touch it.
Your project definition lives in `configure()` and reads almost exactly
like a `CMakeLists.txt`, except it's actual C++ with type safety, IDE
autocomplete, and zero new syntax.

### Top-level `build.cxx`

```cpp
// 2>/dev/null; d=$(dirname "$(readlink -f "$0")"); o="$d/build/.bootstrap"; ... exec "$o" "$@"
#include "build_cxx/build.hxx"
#include "src/core/build.cxx"

constexpr auto configure = []() consteval -> build::project {
    using namespace build;
    using enum standard;
    using enum compiler_id;

    project p("myapp", { .major = 2, .minor = 1 });
    p.cc(clang).std(cpp23).release().werror();

    p.add(src_core);   // compose sub-module (see below)

    p.exe("myapp", { "src/main.cpp" })
        .inc("include")
        .dep("core");

    return p;
};

build::run<configure> app;
int main(int argc, char** argv) { return app(argc, argv); }
```

Line 1 is both a `//` C++ comment and a shell command. After `chmod +x
build.cxx` once, `./build.cxx` self-compiles (cached in
`build/.bootstrap`) and runs. The shebang line tracks the full include
graph via a depfile, so changes to any subdir `build.cxx` correctly
invalidate the bootstrap.

### Sub `build.cxx` (`src/core/build.cxx`)

```cpp
#pragma once
#include "build_cxx/build.hxx"

inline consteval void src_core(build::project& p)
{
    p.lib("core", { "core.cpp", "utils.cpp" }).inc(".");
}
```

One free function per subdir. The parent includes the file and composes
it through the unified compose verb:

```cpp
p.add(src_core);     // or, equivalently: src_core(p);
```

Paths resolve relative to the file that wrote them via
`std::source_location` — the subdir can use `{"core.cpp"}` without
worrying about where the parent is standing.

`p.add(...)` is overloaded for every kind of thing you put into a
project:

- `p.add(src_core)` — a module function (`void(project&)`)
- `p.add(build::exe{ .name="app", .sources={"main.cpp"} })` — a target
  spec (data-first alternative to the builder chain)
- `p.add(build::static_lib{...})`, `p.add(build::shared_lib{...})`

One verb, one spelling for "put this into the project."

## CLI

```bash
./build.cxx                    build everything
./build.cxx --list             list targets and runnables
./build.cxx --target myapp     build one target + its transitive deps
./build.cxx --run test         run a named runnable (see below)
./build.cxx --why              print reason each step rebuilt (CPPBUILD_WHY=1)
./build.cxx --verbose          dep-walk + substitution trace (CPPBUILD_VERBOSE=1)
./build.cxx --dry-run          print plan, don't execute
./build.cxx --clean            remove build dir + compile_commands.json
./build.cxx --clean-artifacts  remove outputs, keep _deps/ + .bootstrap
./build.cxx -j 8               cap per-wave parallel compiles at 8
./build.cxx --emit ninja       write <build_dir>/build.ninja
./build.cxx --emit compile-db  write compile_commands.json only
./build.cxx --quiet            suppress per-step command echo
./build.cxx --help
```

`--clean` nukes everything. `--clean-artifacts` keeps git-cloned deps
(`_deps/`) and the compiled driver (`.bootstrap`) — re-running skips
the SDL3 (or similar) redownload but recompiles your own code.

`--why` prints a one-line reason before each rebuilt step — e.g.
`input newer: src/foo.cpp`, `command hash changed`, `output missing`.
No reason = cached.

`--target foo` is transitive: it builds `foo` plus every library `foo`
depends on. Unknown target = exit 2 with the target list.

## Runnables — test, deploy, package

Runnables are named groups of shell commands addressable by `--run`.
Use them for test suites, packaging, smoke tests, deploys — anything
that isn't an artifact.

There are two shapes, both pure C++26. Use a literal string for the
common case; use a callable (captureless lambda) when you need a path
known only at runtime, and compose it with `std::format`.

```cpp
#include <format>
#include <string>

p.exe("myapp",   { "src/main.cpp" });
p.exe("mytests", { "tests/all.cpp" });

p.runnable("test")
    .dep("mytests")                    // built (transitively) first
    .cmd("build/mytests");             // literal string

p.runnable("deploy")
    .dep("myapp")
    .cmd(+[](build::ctx c) -> std::string {
        return std::format("strip --strip-all {}/myapp", c.build_dir);
    })
    .cmd(+[](build::ctx c) -> std::string {
        return std::format(
            "tar czf {0}/myapp.tar.gz -C {0} myapp", c.build_dir);
    });
```

```bash
./build.cxx --run test     # builds mytests, then runs build/mytests
./build.cxx --run deploy   # builds myapp, strips, tars
```

The driver `chdir()`s to the project root before running commands, so
relative paths work regardless of where the user invoked `build.cxx`
from. `build::ctx` carries the build dir (as a `std::string_view`)
and the project root — everything else you build yourself with
`std::format`. **No invented template syntax.** If you want type
safety on format placeholders, `std::format_string<Args...>`
validates them at compile time already.

Rules:

- Deps are built transitively before commands run.
- Commands run serially, top to bottom. First non-zero exit fails the
  runnable.
- Runnables are **actions**, not artifacts — they run every invocation,
  even when deps are cached.
- Callables must be captureless (convertible to a function pointer).
  If you need state, put it in `build::ctx`.

This is deliberately *not* a packaging DSL. One `std::format` call
beats any DSL we could invent for `tar`/`cp`/`strip`/`zip`. CPack-style
abstractions are avoided on purpose.

First run compiles the driver (≈1.5 s) and caches it in
`build/.bootstrap`. Subsequent runs skip the compile unless the root
`build.cxx` or any of its included files changed — **warm run is ≈40
ms total**. Swap compilers with `CXX=g++ ./build.cxx`. Force a
bootstrap rebuild with `touch build.cxx` or `rm build/.bootstrap`.

For CI or any non-shebang invocation, the long form still works:

```bash
clang++ -std=c++26 -I. build.cxx -o /tmp/_b && /tmp/_b
```

## API

One name per concept. No aliases. Short names are canonical.

### Project

| call                                     | effect                                    |
| ---------------------------------------- | ----------------------------------------- |
| `project("name", { .major=1 })`          | construct                                 |
| `.cc(compiler_id)`                       | default compiler                          |
| `.out("build")`                          | build dir                                 |
| `.std(cpp23)`                            | default C++ standard                      |
| `.release()` / `.debug()`                | default build type                        |
| `.werror()` / `.warnings(false)`         | default warning policy                    |
| `.inc("include", "src")`                 | global `-I` (applied to every target)     |
| `.sys("third_party/foo/include")`        | global `-isystem`                         |
| `.def("FOO=1")`                          | global `-DFOO=1`                          |
| `.flag("-ffast-math")`                   | global compiler flag                      |
| `.exe("name", {"main.cpp"})`             | executable target                         |
| `.lib("name", {"a.cpp"})`                | static library                            |
| `.shared("name", {"a.cpp"})`             | shared library                            |
| `.gen("version.hpp", "literal")`         | generated file, literal content           |
| `.gen("version.hpp", lambda)`            | generated file, content from `gen_ctx`    |
| `.git("name", "url", "rev")`             | fetch git checkout                        |
| `.ext("name")…`                          | external project (CMake / Meson / make)   |
| `.add(module_fn)`                        | compose a subdir module `void(project&)`  |
| `.add(exe{...})` / `add(static_lib{...})`| data-first target spec (aggregate init)   |
| `.pre("cmd")` / `.pre(lambda)`           | project-level hook before build / runnable |
| `.post("cmd")` / `.post(lambda)`         | project-level hook after build / runnable |

### Target (returned by `exe` / `lib` / `shared`)

| call                                     | effect                                    |
| ---------------------------------------- | ----------------------------------------- |
| `.src("a.cpp", "b.cpp")`                 | sources (paths relative to caller's file) |
| `.src_glob("*.cpp")` / `"**/*.cpp"`      | glob, expanded at runtime                 |
| `.src_regex(R"(.+_impl\.cpp)")`          | ECMAScript regex over the caller's dir    |
| `.inc("include")`                        | per-target `-I`                           |
| `.sys("third_party/include")`            | per-target `-isystem`                     |
| `.def("NDEBUG", "KEY=VAL")`              | `-D` list                                 |
| `.flag(...)` / `.lflag(...)`             | compiler / linker flags                   |
| `.dep("core", "utils")`                  | link these libraries                      |
| `.std(cpp26)` / `.release()` / `.debug()`| per-target overrides                      |
| `.werror()` / `.warnings(false)`         | warning policy override                   |
| `.unity()`                               | concatenate sources, compile once         |
| `.soname("libx.so.1")`                   | shared-lib SONAME                         |
| `.before("cmd")` / `.before(lambda)`     | pre-build hook(s), serial                 |
| `.after("cmd")` / `.after(lambda)`       | post-build hook(s), serial                |

Three shapes per kind — inline sources, builder chain, or data-first
aggregate:

```cpp
p.exe("hello", { "main.cpp" });                 // one-liner

p.exe("hello")                                  // builder chain
    .src("main.cpp")
    .inc("include")
    .dep("core");

p.exe("hello", { "main.cpp" })                  // mix
    .inc("include")
    .dep("core");

build::add(p, build::exe{                       // data-first aggregate
    .name     = "hello",
    .sources  = {"main.cpp"},
    .includes = {"include"},
    .deps     = {"core"},
});
```

All four forms are equivalent. Use whichever reads better in context.
The data-first form composes cleanly when you want to generate specs
programmatically (e.g. iterate a `std::array` of target descriptions).

## Ninja backend

`build.cxx` runs the build directly — no configure step, no
intermediate generator. If you need remote caching, IDE debugger
integration, or 10k-file scale, emit Ninja:

```bash
./build.cxx --emit ninja
ninja
```

The Ninja file is a pure function of the consteval plan: no cache on
disk, no regenerate-on-change dance. Run `./build.cxx --emit ninja`
again when your `build.cxx` changes.

That's the only backend. Make, MSBuild, Xcode project emitters are a
maintenance swamp we deliberately refuse to enter.
`compile_commands.json` covers every IDE worth the name (VSCode,
CLion, Qt Creator, Neovim, Emacs with `clangd`).

## Incremental + parallel

Two independent staleness checks, both must pass to skip a step:

1. **mtime** — output newer than every source + every path in its
   depfile (`-MD -MF`). Standard Make-style check.
2. **command fingerprint** — FNV-1a hash of the exact compile command
   matches the stamp written by the last successful run. Stamps live
   under `build/.cache/`. Closes the CMake-classic hole where changing
   `-O2 → -O0` wouldn't rebuild anything because no mtime changed.

Compile steps of the same target run in parallel. Link/archive runs
serially. Cross-target order follows declaration order. `-j N` caps
per-wave parallelism (default: unbounded).

## Unity build

```cpp
p.exe("app", { "a.cpp", "b.cpp", "c.cpp", "main.cpp" }).unity();
```

`build/obj/<target>/unity.cpp` is generated at runtime (content-aware,
so the cache stays warm when sources don't change). Compiler runs once.
`compile_commands.json` still lists each real source individually so
clangd gives per-file diagnostics.

## Glob / regex sources

```cpp
p.exe("app")
    .src_glob("*.cpp")              // non-recursive
    .src_glob("**/*.cpp")           // recursive
    .src_regex(R"(.+_impl\.cpp)");  // ECMAScript regex
```

Anchored to the file that wrote them via `std::source_location`.
Expansion happens at runtime; results are sorted for determinism.
Static `.src(...)` and deferred patterns compose on the same target
(merged, deduped).

## External projects

```cpp
p.git("sdl3",
      "https://github.com/libsdl-org/SDL.git",
      "release-3.2.14");

p.ext("sdl3_cmake")
    .from("sdl3")
    .configure("cmake -S @SRC@ -B @BUILD@ -GNinja")
    .build("cmake --build @BUILD@ --parallel")
    .env("CMAKE_COLOR_DIAGNOSTICS=ON")
    .define("CMAKE_BUILD_TYPE=Release",
            "SDL_SHARED=ON",
            "SDL_STATIC=OFF")
    .produces_shared("libSDL3.so");

p.exe("demo", { "main.cpp" })
    .sys("../../build/_deps/sdl3/src/include")
    .dep("sdl3_cmake");
```

The executor clones into `build/_deps/<name>/src`, runs configure then
build with `@SRC@` / `@BUILD@` / `@ROOT@` substituted, and marks
`.configured` / `.built` for incremental runs. `rm -rf build/_deps` to
force refresh.

> **On sentinels.** `.configure(...)` and `.build(...)` still use the
> `@SRC@` / `@BUILD@` tokens because external builds aren't our code —
> the CMake/Meson/Makefile invocation has its own notion of SRC and
> BUILD directories that differ per-external. Everything *user-facing*
> (runnable commands, post-build hooks, generated files) uses lambdas
> and `std::format`, not sentinels.

`.produces_shared()` / `.produces_static()` register the external as a
virtual link target; consumers get the right `-L` / `-l` / `-Wl,-rpath`
emitted automatically.

## File generation

Two shapes. Literal content for simple cases; a lambda for anything
that embeds runtime-known values, using `std::format` over
`build::gen_ctx`:

```cpp
#include <format>
#include <string>

p.gen("cfg/banner.hpp",
      "#pragma once\nconstexpr auto banner = \"hello\";\n");

p.gen("cfg/version.hpp", +[](build::gen_ctx c) -> std::string {
    return std::format(
        "#pragma once\n"
        "constexpr auto name    = \"{}\";\n"
        "constexpr auto version = \"{}\";\n"
        "constexpr auto root    = \"{}\";\n"
        "constexpr auto commit  = \"deadbeef\";\n",
        c.project, c.version, c.root);
});
```

Files land in `<build_dir>/generated/<rel_path>`, never in the source
tree. That directory is auto-added to every target's `-I` list, so
user code just `#include "cfg/version.hpp"` — no per-target
boilerplate. `gen_ctx` carries `project`, `version`
(`"major.minor.patch"`), and `root`. Content-aware: identical bytes →
mtime stays, downstream incremental cache stays warm.

## Lifecycle hooks

Four slots, two scopes. Symmetric, same API shape (literal string or
captureless `std::string(ctx)` lambda), different firing rules.

### Target scope: `.before(...)` / `.after(...)`

```cpp
#include <format>
#include <string>

p.exe("app", { "main.cpp" })
    .before("mkdir -p logs")                                  // literal
    .before(+[](build::target_ctx c) -> std::string {
        return std::format("echo building {}", c.out);
    })
    .after("size build/app")
    .after(+[](build::target_ctx c) -> std::string {          // dynamic
        return std::format("strip --strip-all {}", c.out);
    });
```

- `.before(...)` fires **once per target per invocation**, just before
  the first non-cached compile step. If the target is fully cached
  (nothing to compile or link), `.before` does not fire.
- `.after(...)` fires after the link/archive step succeeds, only if
  the link actually ran this invocation (not when cached).
- Both run serially in declaration order. Any non-zero exit fails the
  target.
- `target_ctx` carries `out` (artifact path), `build_dir`, `root`.

### Project scope: `p.pre(...)` / `p.post(...)`

```cpp
p.pre(+[](build::project_ctx c) -> std::string {
    return std::format("echo starting {} v{}", c.project, c.version);
});
p.post("echo done");
```

- `p.pre(...)` fires **once per invocation**, before any build work.
- `p.post(...)` fires **once per invocation**, after the build succeeds
  (skipped on failure — failing builds don't print "done").
- Both always run — no cache-skip semantics. They bracket the *intent*
  to build, not per-target work. Use them for banners, timing, audit
  logs, notifications.
- Fire for both `./build.cxx` (build) and `./build.cxx --run foo`
  (runnable). Do not fire for pure-info commands (`--list`, `--help`,
  `--dry-run`, `--clean`).
- `project_ctx` carries `project` (name), `version`, `build_dir`, `root`.

Rule of thumb: if the hook depends on a specific artifact (`strip`,
`size`, `sign`), it belongs on the target. If it's about the whole
build session (banner, timestamp, upload logs), it belongs on the
project.

## Output

Every run prints a banner with what the build system found:

```
● cppbuild v1.0.0
  compiler : /usr/bin/clang++
  version  : clang version 21.1.8
  triple   : x86_64-unknown-linux-gnu
  resource : /usr/lib/llvm-21/lib/clang/21
  sys-incs : 6 paths
```

Every step prints label + status, the exact compiler command (yellow),
and the output path. On failure: red banner with exit code and output.

```
● build complete — 34 step(s): 0 ran, 34 cached  (0.007s)
```

Set `CPPBUILD_QUIET=1` (or pass `--quiet`) to collapse per-step output.

## Errors

Consteval failures go through `consteval_fail()` which embeds a
category tag and a specific message. Compilers quote both verbatim, so
a typo in a dependency name shows:

```
... call to consteval function ... throws
note: ... "[target] unknown dependency — declare the library before its consumer"
```

Runtime failures go through `die()`: one red banner, the file:line,
and `std::exit(1)`.

## Examples

`src/examples/` — one capability per demo. The top-level driver builds
them all:

```
01_hello            single-source executable
02_static_lib       exe + static library dep
03_shared_lib       exe + shared library dep (rpath baked in)
04_unity_build      .unity() — concatenate, compile once
05_glob_sources     .src_glob("*.cpp") — runtime expansion
06_nested_subdirs   arbitrarily nested subdir composition
07_custom_flags     per-target standard / build_type / defines / flags
08_sdl3             fetch SDL3 from GitHub, build with its own CMake,
                    link against libSDL3.so — one command
09_generate_and_postbuild
                    generate a version header (via std::format over gen_ctx),
                    run `size <OUT>` after linking (via target_ctx)
10_app_with_tests   static lib + app + test runner whose post-build
                    runs itself — `./build.cxx` doubles as build-and-test
11_runnables        p.runnable("name") — named shell actions for test,
                    smoke, and packaging (`--run ex_11_pack` → .tar.gz)
```

## What happens at compile time

All of it. Project validation, dependency resolution, flag assembly,
full command-line string construction, compile database entry
generation — every bit runs during compilation of `build.cxx` via
`consteval`. The runtime portion is just: create the build dir, probe
the compiler, expand globs/unity, write `compile_commands.json`,
`std::system()` the pre-baked command strings, print.

## What it generates

```
build/compile_commands.json    for clangd / clang-tidy / IDEs
build/targets.hpp              C++ IDs for every target + runnable
build/<target>                 your actual binaries / libraries
build/generated/...            any .gen() outputs
build/_deps/...                git fetches + external_project outputs
build/.cache/...               content-hash stamp files
build/.bootstrap               cached driver binary
build/build.ninja              only with --emit ninja
compile_commands.json          symlink (or copy) to build/compile_commands.json
.clangd                        auto-generated clangd config
```

## Two-stage, invisibly

The system is already a two-stage build; the stages just don't have
separate user-facing commands.

**Stage 1 — "configure" (compile time).** Your `build.cxx` compiles.
`consteval` runs: project validation, dependency resolution, full
command-line string generation, plan linearization. The result is a
native executable at `build/.bootstrap` whose code *is* the build plan
— every compiler command is a baked string constant. No configure
cache, no `CMakeCache.txt`, no regenerate dance.

**Stage 2 — "build" (runtime).** `build/.bootstrap` runs. Probes the
compiler, writes `compile_commands.json`, expands globs/unity, runs
`std::system()` on the baked command strings, handles incremental
checks. The shebang form just keeps stage 1 fresh: `./build.cxx`
recompiles the driver when any `build.cxx` file in the include graph
changed, then execs `build/.bootstrap` with your argv.

You can run stage 2 directly if the driver is already built:

```bash
./build.cxx                   # stage 1 + stage 2 (shebang keeps .bootstrap fresh)
./build/.bootstrap            # stage 2 only (skip staleness check)
./build/.bootstrap --list     # same CLI as ./build.cxx
./build/.bootstrap --emit ninja
```

There is no separate `cmake -S . -B build` command because there is no
intermediate artefact to materialise — the plan lives inside the
executable itself. Stage 1's output and stage 2's input are the same
file.

## clangd: everything in the compile DB

`build/compile_commands.json` (symlinked to the project root on every
run) contains precise flags for **both** your user code *and* every
`build.cxx` in the tree — root + subdirs. clangd uses per-TU flags
for build system code same as for regular code. No fallback guessing.

The `.clangd` file is still auto-generated and adds `-I<root>
-std=c++26` as a last-resort fallback for any file clangd encounters
before the DB refreshes (e.g. a brand-new subdir `build.cxx` between
`./build.cxx` runs).

### clang-tidy

Because the compile DB covers build.cxx files too, one shell line
lints the whole project including its build system:

```bash
./build.cxx
clang-tidy -p build/ build.cxx build_cxx/*.hxx
```

A project-level `.clang-tidy` suppresses `bugprone-suspicious-include`
(our `.cxx` includes are intentional — the files are polyglot TUs, not
headers).

### Typed target references (`build/targets.hpp`)

Every run generates `build/targets.hpp`, a simple C++ header:

```cpp
namespace targets {
    inline constexpr std::string_view ex_01_hello = "ex_01_hello";
    inline constexpr std::string_view ex_10_calc  = "ex_10_calc";
    // ...
}
namespace runnables {
    inline constexpr std::string_view ex_11_test = "ex_11_test";
    inline constexpr std::string_view selftest   = "selftest";
}
```

User code can include it to get **autocomplete, go-to-definition, and
compile-error-on-typo** instead of raw string literals:

```cpp
#include "build/targets.hpp"
// ... later, in a tool that enumerates your targets ...
auto all = { targets::ex_10_calc, targets::ex_10_app };
```

The `build.cxx` files themselves still use bare strings (`p.exe("foo").dep("bar")`):
their dependency names are validated **at compile time** via
`consteval` anyway, so typos in `.dep(...)` also surface as compiler
errors — the `targets::` header adds value mostly for *downstream*
code that wants to treat the build graph as data.

## Self-testing

The `selftest` runnable runs the compiled driver through its user-facing
surface and checks invariants: `--help`, `--list`, incremental caching,
`--why` output, exit codes on unknown targets/runnables,
`compile_commands.json` shape, generated `targets.hpp` contents.

```bash
./build.cxx --run selftest
# ✓ driver exists
# ✓ --help works
# ✓ --list shows targets + runnables
# ✓ --target ex_01_hello builds
# ✓ rerun is cached
# ✓ --why prints rebuild reason
# ✓ unknown target exits 2
# ✓ unknown runnable exits 2
# ✓ compile_commands.json looks sane
# ✓ targets.hpp declares targets and runnables
# ● all selftests passed
```

Script lives at `src/examples/11_runnables/selftest.sh`. Use it in CI.

## Portability

All OS-specific calls live in `build_cxx/portable.hxx` (3 functions:
`set_env`, `open_pipe`, `close_pipe`, + a `decode_exit` for POSIX
wait-status). Everything else is C++26 standard library. When C++26
`std::process` (P1750) lands in the shipping stdlib, `portable.hxx`
collapses to a one-line re-export.

Tested on Linux + clang-21. macOS should work identically (same POSIX
calls, same clang). Windows: the `_popen`/`_putenv_s` branches compile
but are untested — PRs welcome.

## Why no configure phase? Why no second generator?

CMake has `cmake -S . -B build` (configure) and `cmake --build build`
(build) because its DSL is interpreted at build time and needs a
generator pass to emit real build files (Ninja / Make / MSBuild /
Xcode). Here, **configure is compile.** Running `./build.cxx` the
first time compiles the driver; your `configure` lambda evaluates
during `constexpr`-init, the plan is computed via `make_plan(project)`
at compile time, validation `static_assert`s fire then. There is no
configure step to expose.

A pluggable generator API is how CMake became what it is. We refuse on
principle: generators drift, the public plan shape becomes frozen by
external consumers, and every new feature has to be implemented once
per backend. Ninja is the only generator any serious project actually
uses; we emit it and stop.

## Design notes

- **OOP facade, functional core.** Call sites read as builder OOP
  (`p.exe(...).dep(...)`) — what users already read from CMake.
  Internals are pure functions (`make_plan(project) -> plan`,
  `emit_command(target) -> string`). No mutation behind the facade.
- **No macros anywhere in the user surface.** `consteval` functions,
  builder methods, variable templates, and `build::run<configure>` are
  all plain C++26. The polyglot shebang is a shell feature, not a
  macro — the compiler sees a `//` comment.
- **Minimum user boilerplate (3 lines at the bottom of `build.cxx`):**

  ```cpp
  constexpr auto configure = []() consteval -> build::project { ... };
  build::run<configure> app;
  int main(int argc, char** argv) { return app(argc, argv); }
  ```

  The lambda-by-reference form is how you express "consteval callable
  as NTTP" today — a plain `consteval auto configure()` cannot be used
  because forming a pointer to it is not a constant expression outside
  an immediate-function context (P2564-adjacent gap). `main(argc,
  argv)` is the dispatch point; all work happens there, not in static
  init.

## Limitations

`consteval` in C++ is a pure computation sandbox: no I/O, no processes.
So the runtime shim (executor in `main()`) is unavoidable.

Linux / macOS / any POSIX-ish environment. Windows-native (MSVC) is
not yet supported; clang-cl may work with caveats.

## Requirements

- Clang with `-std=c++26` support (Clang 19+), or GCC with equivalent.
- A POSIX-ish environment (for `std::system`, `std::filesystem`,
  `popen`).
- Ninja is optional — only needed if you use `--emit ninja`.

## Wishlist for the standard

Three narrow relaxations would make the remaining boilerplate
disappear:

1. **Pointer-to-consteval as NTTP.** Lets `build::run<configure>` work
   with a plain `consteval auto configure()` function.
2. **Blessed shebang convention.** A `#!` on line 1 skipped by the
   preprocessor removes the polyglot comment.
3. **Standard-library-driven language mode.** `module std : 26` or a
   `#pragma cpp_std 26` honoured before tokenization removes
   `-std=c++26` from the bootstrap line.

None are blocking. The system works today without them.

## License

Public domain. Do whatever you want with it.
