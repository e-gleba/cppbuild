#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_04_unity_build(build::project& p)
{
    p.exe("ex_04_unity_build", { "a.cpp", "b.cpp", "c.cpp", "main.cpp" })
        .unity();
}
