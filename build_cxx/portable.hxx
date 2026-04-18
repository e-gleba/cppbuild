// build_cxx/portable.hxx
//
// The single place OS-specific calls live. C++26 does not yet ship a
// portable process API (P1750 / std::process isn't in a released
// stdlib), so until then we need popen / setenv — which differ between
// POSIX and MSVC in spelling, not substance. Isolate the 3 calls here;
// everything else compiles on either platform.
//
// API:
//   set_env(key, value)      setenv         / _putenv_s
//   open_pipe(cmd)           popen "r"      / _popen  "r"
//   close_pipe(f)            pclose         / _pclose
//   decode_exit(system_rc)   POSIX wait-status -> exit code;
//                            passthrough on Windows (never returns 0
//                            for non-zero input).
#pragma once

#include <cstdio>
#include <cstdlib>

#if __has_include(<sys/wait.h>)
#    include <sys/wait.h>
#    define CPPBUILD_HAS_WAIT_H 1
#else
#    define CPPBUILD_HAS_WAIT_H 0
#endif

#if defined(_WIN32)
#    include <stdlib.h>
#endif

namespace build::portable {

inline void set_env(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    ::setenv(key, value, 1);
#endif
}

[[nodiscard]] inline auto open_pipe(const char* cmd) -> std::FILE*
{
#if defined(_WIN32)
    return ::_popen(cmd, "r");
#else
    return ::popen(cmd, "r");
#endif
}

inline void close_pipe(std::FILE* f)
{
#if defined(_WIN32)
    (void) ::_pclose(f);
#else
    (void) ::pclose(f);
#endif
}

[[nodiscard]] inline auto decode_exit(int system_rc) -> int
{
    if (system_rc == 0) return 0;
#if CPPBUILD_HAS_WAIT_H
    if (WIFEXITED(system_rc)) {
        const int rc = WEXITSTATUS(system_rc);
        return rc == 0 ? 1 : rc;
    }
    if (WIFSIGNALED(system_rc)) return 128 + WTERMSIG(system_rc);
    return 1;
#else
    return system_rc;
#endif
}

} // namespace build::portable
