#pragma once
#include "build_cxx/build.hxx"

#include <format>
#include <string>

// ─────────────────────────────────────────────────────────────────────────
//  ex_11_runnables — named shell actions via p.runnable("name")
//
//  Runnables are user-defined shell commands you invoke with:
//      ./build.cxx --run <name>
//
//  Unlike targets (which produce artifacts and are cached), runnables
//  are actions — they execute every time. Typical uses: test, deploy,
//  bench, package, docs.
//
//  Each runnable has:
//    .dep(t1, t2, ...)   targets to build first (transitive)
//    .cmd("...")         literal command string
//    .cmd(lambda)        captureless lambda of type
//                          std::string(build::ctx) — build the command
//                          at runtime, typically via std::format.
//
//  build::ctx fields:
//    c.build_dir   the project's build directory (e.g. "build")
//    c.root        absolute path of the project root
//
//  The driver chdir()s to the project root before running, so relative
//  paths work regardless of where the user invoked build.cxx from.
// ─────────────────────────────────────────────────────────────────────────

inline consteval void ex_11_runnables(build::project& p)
{
    p.exe("ex_11_app",  { "main.cpp" });
    p.exe("ex_11_test", { "test_basic.cpp" });

    p.runnable("ex_11_test")
        .dep("ex_11_test")
        .cmd("build/ex_11_test");

    p.runnable("ex_11_smoke")
        .dep("ex_11_app")
        .cmd("build/ex_11_app --smoke")
        .cmd("build/ex_11_app");

    p.runnable("ex_11_pack")
        .dep("ex_11_app")
        .cmd(+[](build::ctx c) -> std::string {
            return std::format(
                "cp -f {0}/ex_11_app {0}/ex_11_app.stripped", c.build_dir);
        })
        .cmd(+[](build::ctx c) -> std::string {
            return std::format(
                "strip --strip-all {}/ex_11_app.stripped || true",
                c.build_dir);
        })
        .cmd(+[](build::ctx c) -> std::string {
            return std::format(
                "tar czf {0}/ex_11_app.tar.gz -C {0} ex_11_app.stripped",
                c.build_dir);
        })
        .cmd(+[](build::ctx c) -> std::string {
            return std::format("echo packaged: {}/ex_11_app.tar.gz",
                               c.build_dir);
        });
}
