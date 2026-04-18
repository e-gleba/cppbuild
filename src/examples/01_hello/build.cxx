#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_01_hello(build::project& p)
{
    p.exe("ex_01_hello", { "main.cpp" });
}
