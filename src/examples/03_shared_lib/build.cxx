#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_03_shared_lib(build::project& p)
{
    p.shared("ex_03_greet", { "src/greet.cpp" })
        .inc({ "include" });
    p.exe("ex_03_shared_lib", { "main.cpp" })
        .inc({ "include" })
        .dep("ex_03_greet");
}
