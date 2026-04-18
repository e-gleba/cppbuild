#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_07_custom_flags(build::project& p)
{
    p.exe("ex_07_custom_flags", { "main.cpp" })
        .std(build::standard::cpp26)
        .debug()
        .def(R"(EX07_GREETING=\"hello-flags\")")
        .flag("-ftemplate-backtrace-limit=0");
}
