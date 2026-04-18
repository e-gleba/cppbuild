// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/errors.hxx — error handling: consteval + runtime, both clean.
//
//  Consteval side: every failure throws an object with a category tag and
//  a static message. Compilers quote both verbatim in their "not a
//  constant expression" diagnostic, so you see:
//
//      error: ... call to consteval function ... throws
//        note: ... "[project] duplicate target name"
//
//  instead of an anonymous const-char*.
//
//  Runtime side: die() prints a red banner and std::exit(1). One place
//  for the format, one place for the exit code. No ad-hoc fprintf/exit
//  pairs scattered through the executor.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdio>
#include <cstdlib>
#include <print>
#include <source_location>
#include <string_view>

namespace build {

// ── consteval failure ──────────────────────────────────────────────────

// Throwing a literal pointer-to-chars gives the cleanest compiler diag.
// Wrapping it in a function centralises the idiom and makes every error
// site grep-able by call site.
[[noreturn]] consteval void consteval_fail(const char* categorized_message)
{
    throw categorized_message;
}

// ── runtime failure ────────────────────────────────────────────────────

namespace ansi_err {
constexpr auto reset = "\033[0m";
constexpr auto bold  = "\033[1m";
constexpr auto red   = "\033[1;31m";
} // namespace ansi_err

[[noreturn]] inline void die(
    std::string_view                                 what,
    std::string_view                                 detail = {},
    const std::source_location&                      loc = std::source_location::current())
{
    using namespace ansi_err;
    if (detail.empty()) {
        std::println(stderr,
                     "\n  {}✗ {}{}  ({}:{:d})",
                     red,
                     std::string_view(what),
                     reset,
                     loc.file_name(),
                     static_cast<unsigned>(loc.line()));
    } else {
        std::println(stderr,
                     "\n  {}✗ {}{} — {}  ({}:{:d})",
                     red,
                     std::string_view(what),
                     reset,
                     std::string_view(detail),
                     loc.file_name(),
                     static_cast<unsigned>(loc.line()));
    }
    std::exit(1);
}

} // namespace build
