#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_06_nested_lib(build::project& p)
{
    p.lib("ex_06_util", { "util.cpp" }).inc({ "." });
}
