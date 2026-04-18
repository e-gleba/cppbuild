// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/target.hxx — target = sources + flags + deps.
//
//  One name per concept. No aliases.
//
//  Paths (sources, include dirs) are stored as owning ct_path values and
//  are resolved relative to the directory of the .cxx file that called
//  .src / .inc / .sys. Resolution uses std::source_location captured by
//  the implicit `here_path` constructor, so call sites read like
//  `.src("hello.cpp")` regardless of where the user is standing in the
//  source tree.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "config.hxx"
#include "primitives.hxx"

#include <concepts>
#include <initializer_list>
#include <string>
#include <string_view>

namespace build {

// Forward declared in project.hxx. Re-declared here so target can hold
// post_build_spec values without pulling project.hxx (which pulls us).
struct target_ctx;
using target_cmd_fn = std::string (*)(target_ctx);

struct post_spec;  // defined in project.hxx

struct target final
{
    std::string_view       name_{};
    target_kind            kind_{ target_kind::executable };
    standard               standard_{ standard::cpp23 };
    build_type             build_type_{ build_type::release };
    path_list              sources_{}, include_dirs_{}, system_include_dirs_{};
    string_list            definitions_{};
    string_list            compiler_flags_{}, linker_flags_{}, dependencies_{};
    ct_vector<ct_string<512>, 16> glob_patterns_{};
    std::string_view       soname_{};
    // post_build commands: each is a literal string_view OR a function
    // pointer taking target_ctx. Discriminated by fn != nullptr.
    struct post_entry final {
        std::string_view literal{};
        target_cmd_fn    fn{ nullptr };
    };
    ct_vector<post_entry, 8> post_build_{};
    ct_vector<post_entry, 8> pre_build_{};
    bool                   wall_{ true }, wextra_{ true }, pedantic_{ true };
    bool                   werror_{ false };
    bool                   unity_{ false };

    // ── Sources ───────────────────────────────────────────────────────
    consteval auto& src(std::initializer_list<here_path> xs)
    {
        for (auto& x : xs) sources_.push_back(x.resolved());
        return *this;
    }
    consteval auto& src(here_path p)
    {
        sources_.push_back(p.resolved());
        return *this;
    }

    // Deferred source patterns — expanded at runtime against the
    // directory of the file that wrote them.
    //
    //   .src_glob("*.cpp")               non-recursive
    //   .src_glob("**/*.cpp")            recursive
    //   .src_regex(R"(.+_impl\.cpp$)")   ECMAScript regex
    consteval auto& src_glob(
        const char* pattern,
        std::source_location loc = std::source_location::current())
    {
        glob_patterns_.push_back(
            here_pattern(pattern, pattern_kind::glob, loc).serialized());
        return *this;
    }
    consteval auto& src_regex(
        const char* pattern,
        std::source_location loc = std::source_location::current())
    {
        glob_patterns_.push_back(
            here_pattern(pattern, pattern_kind::regex, loc).serialized());
        return *this;
    }

    // ── Include dirs ──────────────────────────────────────────────────
    consteval auto& inc(std::initializer_list<here_path> xs)
    {
        for (auto& x : xs) include_dirs_.push_back(x.resolved());
        return *this;
    }
    consteval auto& inc(here_path p)
    {
        include_dirs_.push_back(p.resolved());
        return *this;
    }
    // SYSTEM include dirs: same as -I but emitted as -isystem. Third-party
    // headers don't pollute warnings into your own code.
    consteval auto& sys(std::initializer_list<here_path> xs)
    {
        for (auto& x : xs) system_include_dirs_.push_back(x.resolved());
        return *this;
    }
    consteval auto& sys(here_path p)
    {
        system_include_dirs_.push_back(p.resolved());
        return *this;
    }

    // ── Flags / defs / deps ───────────────────────────────────────────
    template <std::convertible_to<std::string_view>... A>
    consteval auto& def(A... d)
    {
        (definitions_.push_back(std::string_view(d)), ...);
        return *this;
    }
    template <std::convertible_to<std::string_view>... A>
    consteval auto& flag(A... f)
    {
        (compiler_flags_.push_back(std::string_view(f)), ...);
        return *this;
    }
    template <std::convertible_to<std::string_view>... A>
    consteval auto& lflag(A... f)
    {
        (linker_flags_.push_back(std::string_view(f)), ...);
        return *this;
    }
    template <std::convertible_to<std::string_view>... A>
    consteval auto& dep(A... l)
    {
        (dependencies_.push_back(std::string_view(l)), ...);
        return *this;
    }

    // ── Modifiers ─────────────────────────────────────────────────────
    consteval auto& std(standard s)          { standard_ = s; return *this; }
    consteval auto& release()                { build_type_ = build_type::release; return *this; }
    consteval auto& debug()                  { build_type_ = build_type::debug; return *this; }
    consteval auto& werror(bool v = true)    { werror_ = v; return *this; }
    consteval auto& warnings(bool v = true)  { wall_ = wextra_ = pedantic_ = v; return *this; }
    consteval auto& unity(bool v = true)     { unity_ = v; return *this; }
    consteval auto& soname(std::string_view s) { soname_ = s; return *this; }

    // Shell commands around this target's build.
    //
    //   .before("mkdir -p logs")             // runs once before compile
    //   .after("size build/app")             // runs once after link
    //
    //   .before([](build::target_ctx c) {    // callable form
    //       return std::format("echo building {}", c.out);
    //   });
    //   .after([](build::target_ctx c) {
    //       return std::format("strip --strip-all {}", c.out);
    //   });
    //
    // Serial, declaration order. Non-zero exit fails the target.
    // Skipped when the target is fully cached (nothing compiled or
    // linked on this invocation), since hooks bracket real work.
    // For unconditional run-every-time behavior, use project-level
    // p.pre / p.post instead.
    template <std::convertible_to<std::string_view>... A>
        requires (sizeof...(A) > 0)
    consteval auto& before(A... cmds)
    {
        (pre_build_.push_back({ .literal = std::string_view(cmds) }), ...);
        return *this;
    }
    consteval auto& before(target_cmd_fn f)
    {
        pre_build_.push_back({ .fn = f });
        return *this;
    }
    template <std::convertible_to<std::string_view>... A>
        requires (sizeof...(A) > 0)
    consteval auto& after(A... cmds)
    {
        (post_build_.push_back({ .literal = std::string_view(cmds) }), ...);
        return *this;
    }
    consteval auto& after(target_cmd_fn f)
    {
        post_build_.push_back({ .fn = f });
        return *this;
    }
};

} // namespace build
