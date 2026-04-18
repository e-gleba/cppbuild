#pragma once
#include "build_cxx/build.hxx"
#include "app/build.cxx"
#include "lib/build.cxx"

inline consteval void ex_06_nested(build::project& p)
{
    ex_06_nested_lib(p);   // lib must appear before app (consumers after producers)
    ex_06_nested_app(p);
}
