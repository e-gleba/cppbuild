#pragma once
#include "build_cxx/build.hxx"

#include <format>
#include <string>

inline consteval void ex_10_app_with_tests(build::project& p)
{
    p.gen("ex10/version.hpp", +[](build::gen_ctx c) -> std::string {
        return std::format(
            "#pragma once\n"
            "namespace ex10 {{\n"
            "    constexpr auto project = \"{}\";\n"
            "    constexpr auto version = \"{}\";\n"
            "}}\n",
            c.project, c.version);
    });

    p.lib("ex_10_calc", { "src/calc.cpp" }).inc({ "include" });

    p.exe("ex_10_app", { "src/main.cpp" })
        .inc({ "include" })
        .dep("ex_10_calc");

    p.exe("ex_10_tests", { "tests/test_calc.cpp" })
        .inc({ "include" })
        .dep("ex_10_calc")
        .after(+[](build::target_ctx c) -> std::string {
            return std::string(c.out);
        });
}
