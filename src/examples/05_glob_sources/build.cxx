#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_05_glob_sources(build::project& p)
{
    p.exe("ex_05_glob_sources").src_glob("*.cpp");
}
