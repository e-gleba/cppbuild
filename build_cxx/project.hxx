// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/project.hxx — project = targets + globals + defaults.
//
//  API surface for users (everything below is standard C++26):
//
//      p.exe("app", { "main.cpp" }).inc("include").dep("mylib");
//      p.lib("mylib", { "lib.cpp" });
//      p.runnable("test").dep("app").cmd("./build/app");
//
//  No sentinels, no @@ strings. When a command or post-build hook needs
//  a path that depends on the runtime environment (build dir, target
//  output, project root), pass a lambda:
//
//      p.runnable("pack").dep("app")
//          .cmd([](build::ctx c) {
//              return std::format("tar czf {0}/app.tar.gz -C {0} app",
//                                 c.build_dir);
//          });
//
//      p.exe("tests", { "t.cpp" })
//          .after([](build::target_ctx c) { return std::string(c.out); });
//
//  Callables must be captureless (convertible to a plain function
//  pointer). If you want to parameterize, read the ctx fields — they
//  carry every runtime-only value the command could need.
//
//  Data-first alternative (no builder chain):
//
//      build::add(p, build::exe{ .name="app", .sources={"main.cpp"} });
//      build::add(p, build::static_lib{ .name="math",
//                                       .sources={"add.cpp","mul.cpp"} });
//
//  Subdir modules: a module is a consteval function taking `project&`.
//  Include the subdir's build.cxx and either call it directly —
//  `src_core(p);` — or run it through the unified compose verb:
//
//      p.add(src_core);
//      p.add(src_tests);
//
//  `add` is one verb for everything you put into a project: target
//  specs, library specs, and module functions.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "primitives.hxx"
#include "target.hxx"

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>

namespace build {

// ── Contexts passed to user callables ──────────────────────────────────
//
// ctx         — for runnable commands. Carries values known only at
//               execute time (the project's build dir + root).
// target_ctx  — for target.after(...) post-build hooks. Carries the
//               artifact's output path.
// gen_ctx     — for p.gen(name, callable). Carries project name, version
//               string, and root dir so generated headers can embed them.
//
// Each ctx is a plain aggregate filled in by the executor just before
// invoking the callable. Fields are std::string_view into values that
// outlive the call — no ownership games.

struct ctx final
{
    std::string_view build_dir{};
    std::string_view root{};
};

struct target_ctx final
{
    std::string_view out{};        // absolute-ish artifact path
    std::string_view build_dir{};
    std::string_view root{};
};

struct gen_ctx final
{
    std::string_view project{};
    std::string_view version{};    // "MAJOR.MINOR.PATCH"
    std::string_view root{};
};

// Project-level lifecycle hooks (p.pre / p.post). Run once per build
// invocation, outside the per-target plan. Run every invocation — not
// skipped on cache hits, because they bracket the *intent* to build,
// not per-target work.
struct project_ctx final
{
    std::string_view project{};
    std::string_view version{};
    std::string_view build_dir{};
    std::string_view root{};
};

// ── Callable-or-literal discriminated unions ───────────────────────────
//
// Each *_spec is either a static string (literal case, zero overhead) or
// a captureless lambda / function pointer. Construction is via overload
// resolution — user writes .cmd("echo hi") or .cmd([](ctx c){...}) and
// the right spec is chosen.

using cmd_fn          = std::string (*)(ctx);
using target_cmd_fn   = std::string (*)(target_ctx);
using gen_body_fn     = std::string (*)(gen_ctx);
using project_cmd_fn  = std::string (*)(project_ctx);

struct cmd_spec final
{
    std::string_view literal{};
    cmd_fn           fn{ nullptr };

    consteval cmd_spec() = default;
    consteval cmd_spec(std::string_view s) : literal(s) {}
    consteval cmd_spec(const char* s) : literal(s) {}
    consteval cmd_spec(cmd_fn f) : fn(f) {}

    [[nodiscard]] constexpr auto is_dynamic() const -> bool
    {
        return fn != nullptr;
    }
};

struct post_spec final
{
    std::string_view literal{};
    target_cmd_fn    fn{ nullptr };

    consteval post_spec() = default;
    consteval post_spec(std::string_view s) : literal(s) {}
    consteval post_spec(const char* s) : literal(s) {}
    consteval post_spec(target_cmd_fn f) : fn(f) {}

    [[nodiscard]] constexpr auto is_dynamic() const -> bool
    {
        return fn != nullptr;
    }
};

struct project_hook_spec final
{
    std::string_view literal{};
    project_cmd_fn   fn{ nullptr };

    consteval project_hook_spec() = default;
    consteval project_hook_spec(std::string_view s) : literal(s) {}
    consteval project_hook_spec(const char* s) : literal(s) {}
    consteval project_hook_spec(project_cmd_fn f) : fn(f) {}

    [[nodiscard]] constexpr auto is_dynamic() const -> bool
    {
        return fn != nullptr;
    }
};

// ── External projects ──────────────────────────────────────────────────
//
// Two layers:
//
// 1. git: declare a source checkout. The executor clones the repo at the
//    pinned revision into build/_deps/<name>/src if absent.
//
// 2. ext: build something that already ships its own build system
//    (CMake, Meson, Makefile). configure/build commands reference
//    paths via @SRC@ / @BUILD@ / @ROOT@ sentinels — these are scoped
//    to the external-project subsystem (not user-facing) so the
//    invariants of consuming a foreign build system stay explicit.
//    User-facing runnable commands are callable-based (no sentinels).

struct fetch_git_spec final
{
    std::string_view name{};
    std::string_view url{};
    std::string_view rev{};
};

enum class external_kind : unsigned char
{
    none,
    shared_library,
    static_library,
};

struct external_project_spec final
{
    std::string_view name{};
    std::string_view source_name{};
    std::string_view configure_cmd{};
    std::string_view build_cmd{};
    std::string_view produces_file{};
    external_kind    kind{ external_kind::none };
    path_list        include_dirs{};
    string_list      env_vars{};
    string_list      defines{};
};

// ── Generated files ────────────────────────────────────────────────────
//
// Output lives under <build_dir>/generated/<rel_path>, which is on every
// target's -I list. Content is either a literal body or a callable that
// returns the body given a gen_ctx (use std::format inside).
//
//   p.gen("version.hpp",
//         "#pragma once\nconstexpr int v = 1;\n");
//
//   p.gen("version.hpp", [](build::gen_ctx c) {
//       return std::format(
//           "#pragma once\n"
//           "constexpr auto project = \"{}\";\n"
//           "constexpr auto version = \"{}\";\n",
//           c.project, c.version);
//   });
//
// Content-aware: identical bytes → file on disk is left alone, so
// mtime-driven downstream incremental builds don't get poked.

struct generated_file_spec final
{
    ct_path          rel_path{};
    ct_string<8192>  literal_content{};
    gen_body_fn      fn{ nullptr };

    [[nodiscard]] constexpr auto is_dynamic() const -> bool
    {
        return fn != nullptr;
    }
};

// ── Runnables ──────────────────────────────────────────────────────────
//
// A named group of shell commands invoked via `./build.cxx --run <name>`.
// Not part of the build plan, not cached. Typical uses: test, deploy,
// package, bench, docs.
//
//   p.runnable("smoke")
//       .dep("app")
//       .cmd("./build/app --smoke");
//
//   p.runnable("pack").dep("app")
//       .cmd([](build::ctx c) {
//           return std::format("tar czf {0}/app.tar.gz -C {0} app",
//                              c.build_dir);
//       });
//
// Deps are built transitively first. Commands run serially in written
// order; any non-zero exit fails the runnable.

struct runnable_spec final
{
    std::string_view              name{};
    string_list                   deps{};
    ct_vector<cmd_spec, 32>       cmds{};
};

struct runnable_builder final
{
    runnable_spec* ref_{};

    template <std::convertible_to<std::string_view>... A>
    consteval auto& dep(A... ts)
    {
        (ref_->deps.push_back(std::string_view(ts)), ...);
        return *this;
    }

    // Overload 1: one-or-more literal strings.
    template <std::convertible_to<std::string_view>... A>
        requires (sizeof...(A) > 0 && (... && std::convertible_to<A, std::string_view>))
    consteval auto& cmd(A... cs)
    {
        (ref_->cmds.push_back(cmd_spec{ std::string_view(cs) }), ...);
        return *this;
    }

    // Overload 2: a captureless lambda / function pointer.
    consteval auto& cmd(cmd_fn f)
    {
        ref_->cmds.push_back(cmd_spec{ f });
        return *this;
    }
};

struct external_project_builder final
{
    external_project_spec* ref_{};

    consteval auto& from(std::string_view src)
    { ref_->source_name = src; return *this; }
    consteval auto& configure(std::string_view cmd)
    { ref_->configure_cmd = cmd; return *this; }
    consteval auto& build(std::string_view cmd)
    { ref_->build_cmd = cmd; return *this; }
    consteval auto& produces_shared(std::string_view rel)
    {
        ref_->produces_file = rel;
        ref_->kind          = external_kind::shared_library;
        return *this;
    }
    consteval auto& produces_static(std::string_view rel)
    {
        ref_->produces_file = rel;
        ref_->kind          = external_kind::static_library;
        return *this;
    }
    consteval auto& sys(std::initializer_list<here_path> xs)
    {
        for (const auto& x : xs) ref_->include_dirs.push_back(x.resolved());
        return *this;
    }
    template <std::convertible_to<std::string_view>... A>
    consteval auto& env(A... kvs)
    { (ref_->env_vars.push_back(std::string_view(kvs)), ...); return *this; }
    template <std::convertible_to<std::string_view>... A>
    consteval auto& define(A... kvs)
    { (ref_->defines.push_back(std::string_view(kvs)), ...); return *this; }
};

// ── Data-first spec types (Stepanov / functional flavor) ───────────────
//
// For users who prefer aggregates over builder chains. Compose a spec
// as a value, then hand it to build::add(p, spec). Equivalent to the
// builder forms — pick whichever reads better in context.

struct exe final
{
    std::string_view name{};
    std::initializer_list<here_path> sources{};
    std::initializer_list<here_path> includes{};
    std::initializer_list<here_path> system_includes{};
    std::initializer_list<std::string_view> defines{};
    std::initializer_list<std::string_view> flags{};
    std::initializer_list<std::string_view> deps{};
};

struct static_lib final
{
    std::string_view name{};
    std::initializer_list<here_path> sources{};
    std::initializer_list<here_path> includes{};
    std::initializer_list<here_path> system_includes{};
    std::initializer_list<std::string_view> defines{};
    std::initializer_list<std::string_view> flags{};
    std::initializer_list<std::string_view> deps{};
};

struct shared_lib final
{
    std::string_view name{};
    std::initializer_list<here_path> sources{};
    std::initializer_list<here_path> includes{};
    std::initializer_list<here_path> system_includes{};
    std::initializer_list<std::string_view> defines{};
    std::initializer_list<std::string_view> flags{};
    std::initializer_list<std::string_view> lflags{};
    std::initializer_list<std::string_view> deps{};
    std::string_view soname{};
};

// Forward decl — defined after project so we can call its methods.
struct project;
consteval auto add(project& p, const exe&)         -> target&;
consteval auto add(project& p, const static_lib&)  -> target&;
consteval auto add(project& p, const shared_lib&)  -> target&;

// ── The project ────────────────────────────────────────────────────────

struct project final
{
    std::string_view                     name_{};
    version                              version_{};
    compiler_id                          compiler_{ compiler_id::clang };
    std::string_view                     build_dir_{ "build" };
    standard                             default_standard_{ standard::cpp23 };
    build_type                           default_build_type_{ build_type::release };
    bool                                 default_warnings_{ true };
    bool                                 default_werror_{ false };
    ct_vector<target, 64>                targets_{};
    ct_vector<fetch_git_spec, 16>        fetches_{};
    ct_vector<external_project_spec, 16> externals_{};
    ct_vector<generated_file_spec, 16>   generated_{};
    ct_vector<runnable_spec, 32>         runnables_{};
    ct_vector<project_hook_spec, 8>      pre_hooks_{};
    ct_vector<project_hook_spec, 8>      post_hooks_{};
    path_list                            global_include_dirs_{};
    path_list                            global_system_include_dirs_{};
    string_list                          global_flags_{}, global_definitions_{};

    consteval project() = default;
    consteval project(std::string_view n, version v)
        : name_(n), version_(v) {}

    // ── Globals / defaults ─────────────────────────────────────────────
    consteval auto& cc(compiler_id c)        { compiler_ = c; return *this; }
    consteval auto& out(std::string_view d)  { build_dir_ = d; return *this; }
    consteval auto& std(standard s)          { default_standard_ = s; return *this; }
    consteval auto& release()                { default_build_type_ = build_type::release; return *this; }
    consteval auto& debug()                  { default_build_type_ = build_type::debug; return *this; }
    consteval auto& werror(bool v = true)    { default_werror_ = v; return *this; }
    consteval auto& warnings(bool v = true)  { default_warnings_ = v; return *this; }

    template <std::convertible_to<std::string_view>... A>
    consteval auto& flag(A... f)
    { (global_flags_.push_back(std::string_view(f)), ...); return *this; }
    template <std::convertible_to<std::string_view>... A>
    consteval auto& def(A... d)
    { (global_definitions_.push_back(std::string_view(d)), ...); return *this; }

    consteval auto& inc(std::initializer_list<here_path> xs)
    {
        for (auto& x : xs) global_include_dirs_.push_back(x.resolved());
        return *this;
    }
    consteval auto& inc(here_path p)
    { global_include_dirs_.push_back(p.resolved()); return *this; }
    consteval auto& sys(std::initializer_list<here_path> xs)
    {
        for (auto& x : xs) global_system_include_dirs_.push_back(x.resolved());
        return *this;
    }
    consteval auto& sys(here_path p)
    { global_system_include_dirs_.push_back(p.resolved()); return *this; }

private:
    consteval void apply_defaults(target& t) const
    {
        t.standard_   = default_standard_;
        t.build_type_ = default_build_type_;
        t.wall_ = t.wextra_ = t.pedantic_ = default_warnings_;
        t.werror_                         = default_werror_;
    }

    [[nodiscard]] consteval auto& emplace(target_kind k, std::string_view n)
    {
        target t;
        t.name_ = n;
        t.kind_ = k;
        apply_defaults(t);
        targets_.push_back(t);
        return targets_[targets_.size() - 1];
    }

public:
    // Builder-style target factories (inline-sources common case):
    //   p.exe("app", {"main.cpp"});          one-liner
    //   p.exe("app").src("main.cpp").inc("inc");  chain
    //
    // Bare forms are [[nodiscard]] — name-only is always a mistake.
    [[nodiscard]] consteval auto& exe(std::string_view n)
    { return emplace(target_kind::executable, n); }
    consteval auto& exe(std::string_view n,
                        std::initializer_list<here_path> srcs)
    { return emplace(target_kind::executable, n).src(srcs); }

    [[nodiscard]] consteval auto& lib(std::string_view n)
    { return emplace(target_kind::static_library, n); }
    consteval auto& lib(std::string_view n,
                        std::initializer_list<here_path> srcs)
    { return emplace(target_kind::static_library, n).src(srcs); }

    [[nodiscard]] consteval auto& shared(std::string_view n)
    { return emplace(target_kind::shared_library, n); }
    consteval auto& shared(std::string_view n,
                           std::initializer_list<here_path> srcs)
    { return emplace(target_kind::shared_library, n).src(srcs); }

    // ── Composition ───────────────────────────────────────────────────
    //
    // A module is a plain consteval function that takes `project&` and
    // mutates it. Parents include the subdir's build.cxx and either
    // call the function directly:
    //
    //     src_core(p);
    //
    // or run it through `p.add(fn)` for a more uniform spelling:
    //
    //     p.add(src_core);
    //
    // Both are equivalent. `add` is the unified compose verb —
    // `p.add(module_fn)`, `p.add(exe{...})`, `p.add(static_lib{...})`
    // all add things to the project.
    template <typename F>
        requires requires (F f, project& pr) { f(pr); }
    consteval auto& add(F&& f)
    {
        f(*this);
        return *this;
    }

    // Data-first overloads (forward-declared; defined after struct).
    // Use `struct <tag>` because the member functions `exe()`, `lib()`,
    // `shared()` create name hiding for the struct tags within the
    // class scope.
    consteval auto& add(const struct exe&);
    consteval auto& add(const struct static_lib&);
    consteval auto& add(const struct shared_lib&);

    // ── Lifecycle hooks ───────────────────────────────────────────────
    //
    // Project-wide before/after callbacks. Run once per invocation,
    // outside the per-target build plan. Run every invocation (not
    // skipped on cache hits). First non-zero exit fails the build
    // without running any later hooks or targets.
    //
    //   p.pre("echo starting build");
    //   p.post(+[](build::project_ctx c) -> std::string {
    //       return std::format("echo done: outputs in {}", c.build_dir);
    //   });
    template <std::convertible_to<std::string_view>... A>
        requires (sizeof...(A) > 0)
    consteval auto& pre(A... cs)
    {
        (pre_hooks_.push_back(project_hook_spec{ std::string_view(cs) }), ...);
        return *this;
    }
    consteval auto& pre(project_cmd_fn f)
    {
        pre_hooks_.push_back(project_hook_spec{ f });
        return *this;
    }
    template <std::convertible_to<std::string_view>... A>
        requires (sizeof...(A) > 0)
    consteval auto& post(A... cs)
    {
        (post_hooks_.push_back(project_hook_spec{ std::string_view(cs) }), ...);
        return *this;
    }
    consteval auto& post(project_cmd_fn f)
    {
        post_hooks_.push_back(project_hook_spec{ f });
        return *this;
    }

    // Generate a file under <build_dir>/generated/<rel_path>. Content can
    // be a literal or a callable taking gen_ctx.
    consteval auto& gen(std::string_view rel_path, std::string_view content)
    {
        generated_file_spec g;
        g.rel_path.append(rel_path);
        g.literal_content.append(content);
        generated_.push_back(g);
        return *this;
    }
    consteval auto& gen(std::string_view rel_path, gen_body_fn f)
    {
        generated_file_spec g;
        g.rel_path.append(rel_path);
        g.fn = f;
        generated_.push_back(g);
        return *this;
    }

    consteval auto& git(std::string_view name,
                        std::string_view url,
                        std::string_view rev)
    { fetches_.push_back({ .name = name, .url = url, .rev = rev }); return *this; }

    [[nodiscard]] consteval auto ext(std::string_view n)
        -> external_project_builder
    {
        external_project_spec s;
        s.name = n;
        externals_.push_back(s);
        return { &externals_[externals_.size() - 1] };
    }

    [[nodiscard]] consteval auto runnable(std::string_view n)
        -> runnable_builder
    {
        runnable_spec s;
        s.name = n;
        runnables_.push_back(s);
        return { &runnables_[runnables_.size() - 1] };
    }

    [[nodiscard]] constexpr auto find_runnable(std::string_view n) const
        -> const runnable_spec*
    {
        for (const auto& r : runnables_) if (r.name == n) return &r;
        return nullptr;
    }

    [[nodiscard]] constexpr auto find_target(std::string_view n) const
        -> const target*
    {
        for (const auto& t : targets_) if (t.name_ == n) return &t;
        return nullptr;
    }
    [[nodiscard]] constexpr auto find_external(std::string_view n) const
        -> const external_project_spec*
    {
        for (const auto& e : externals_) if (e.name == n) return &e;
        return nullptr;
    }
    [[nodiscard]] constexpr auto find_target_index(std::string_view n) const
        -> std::size_t
    {
        for (std::size_t i = 0; i < targets_.size(); ++i) {
            if (targets_[i].name_ == n) return i;
        }
        return npos;
    }
};

// ── Data-first add() free functions ───────────────────────────────────
//
// For users who prefer aggregate specs over builder chains:
//
//     build::add(p, build::exe{ .name="app", .sources={"main.cpp"} });
//
// Equivalent to `p.exe("app", {"main.cpp"})`. Pick whichever reads
// better in context.

namespace detail {

template <typename Spec>
consteval void apply_common(target& t, const Spec& s)
{
    for (auto& x : s.sources)         t.sources_.push_back(x.resolved());
    for (auto& x : s.includes)        t.include_dirs_.push_back(x.resolved());
    for (auto& x : s.system_includes) t.system_include_dirs_.push_back(x.resolved());
    for (auto sv : s.defines)         t.definitions_.push_back(sv);
    for (auto sv : s.flags)           t.compiler_flags_.push_back(sv);
    for (auto sv : s.deps)            t.dependencies_.push_back(sv);
}

} // namespace detail

consteval auto add(project& p, const exe& s) -> target&
{
    auto& t = p.exe(s.name);
    detail::apply_common(t, s);
    return t;
}
consteval auto add(project& p, const static_lib& s) -> target&
{
    auto& t = p.lib(s.name);
    detail::apply_common(t, s);
    return t;
}
consteval auto add(project& p, const shared_lib& s) -> target&
{
    auto& t = p.shared(s.name);
    detail::apply_common(t, s);
    for (auto sv : s.lflags) t.linker_flags_.push_back(sv);
    if (!s.soname.empty()) t.soname_ = s.soname;
    return t;
}

// Member-form data-first add: p.add(exe{...}) mirrors build::add(p, exe{...}).
consteval auto& project::add(const struct exe& s)
{ build::add(*this, s); return *this; }
consteval auto& project::add(const struct static_lib& s)
{ build::add(*this, s); return *this; }
consteval auto& project::add(const struct shared_lib& s)
{ build::add(*this, s); return *this; }

} // namespace build
