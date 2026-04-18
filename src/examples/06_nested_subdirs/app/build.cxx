#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_06_nested_app(build::project& p)
{
    p.exe("ex_06_nested_subdirs", { "main.cpp" })
        .inc({ "../lib" })
        .dep("ex_06_util");
}
