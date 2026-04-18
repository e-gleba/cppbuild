#pragma once
#include "build_cxx/build.hxx"

#include <format>
#include <string>

// Target-level lifecycle hooks: .before(...) fires before the first
// non-cached step of the target, .after(...) fires after link/archive.
// Both are skipped when the target is fully cached. For hooks that
// must run every invocation regardless of cache state, use project-
// level p.pre() / p.post() in the root build.cxx.

inline consteval void ex_09_generate_and_postbuild(build::project& p)
{
    p.gen("ex09/version.hpp", +[](build::gen_ctx c) -> std::string {
        return std::format(
            "#pragma once\n"
            "namespace ex09 {{\n"
            "    constexpr auto project = \"{}\";\n"
            "    constexpr auto version = \"{}\";\n"
            "    constexpr auto root    = \"{}\";\n"
            "    constexpr auto author  = \"egleba\";\n"
            "    constexpr auto commit  = \"deadbeef0000\";\n"
            "}}\n",
            c.project, c.version, c.root);
    });

    p.exe("ex_09_generate_and_postbuild", { "main.cpp" })
        .before(+[](build::target_ctx c) -> std::string {
            return std::format("echo building {}", c.out);
        })
        .after(+[](build::target_ctx c) -> std::string {
            return std::format("size {}", c.out);
        });
}
