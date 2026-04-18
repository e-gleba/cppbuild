#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_02_static_lib(build::project& p)
{
    p.lib("ex_02_mathx", { "src/add.cpp", "src/mul.cpp" })
        .inc({ "include" });
    p.exe("ex_02_static_lib", { "main.cpp" })
        .inc({ "include" })
        .dep("ex_02_mathx");
}
