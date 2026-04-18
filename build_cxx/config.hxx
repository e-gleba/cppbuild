// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/config.hxx — enums + their trivial projections to flags
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "errors.hxx"

#include <string_view>

namespace build {

enum class standard : unsigned
{
    cpp17,
    cpp20,
    cpp23,
    cpp26
};
enum class build_type : unsigned
{
    debug,
    release,
    release_debug_info,
    min_size
};
enum class compiler_id : unsigned
{
    clang,
    gcc
};
enum class target_kind : unsigned
{
    executable,
    static_library,
    shared_library
};

struct version final
{
    int major = 0, minor = 0, patch = 0;
};
struct opt_flags final
{
    std::string_view opt, def;
};

[[nodiscard]] consteval auto to_flag(standard s) -> std::string_view
{
    switch (s) {
        case standard::cpp17:
            return "-std=c++17";
        case standard::cpp20:
            return "-std=c++20";
        case standard::cpp23:
            return "-std=c++23";
        case standard::cpp26:
            return "-std=c++26";
    }
    consteval_fail("[config] to_flag(standard): unhandled enumerator");
}

[[nodiscard]] consteval auto to_flag(compiler_id c) -> std::string_view
{
    switch (c) {
        case compiler_id::clang:
            return "clang++";
        case compiler_id::gcc:
            return "g++";
    }
    consteval_fail("[config] to_flag(compiler_id): unhandled enumerator");
}

[[nodiscard]] consteval auto to_flags(build_type b) -> opt_flags
{
    switch (b) {
        case build_type::debug:
            return { .opt = "-O0 -g", .def = "" };
        case build_type::release:
            return { .opt = "-O2", .def = "-DNDEBUG" };
        case build_type::release_debug_info:
            return { .opt = "-O2 -g", .def = "-DNDEBUG" };
        case build_type::min_size:
            return { .opt = "-Os", .def = "-DNDEBUG" };
    }
    consteval_fail("[config] to_flags(build_type): unhandled enumerator");
}

[[nodiscard]] constexpr auto to_output_ext(target_kind k) -> std::string_view
{
    switch (k) {
        case target_kind::executable:
            return "";
        case target_kind::static_library:
            return ".a";
        case target_kind::shared_library:
            return ".so";
    }
    return ""; // unreachable for well-formed plans; keeps this constexpr
}

} // namespace build
