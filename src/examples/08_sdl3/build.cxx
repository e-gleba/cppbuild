#pragma once
#include "build_cxx/build.hxx"

inline consteval void ex_08_sdl3(build::project& p)
{
    p.git("sdl3",
          "https://github.com/libsdl-org/SDL.git",
          "release-3.2.14");

    p.ext("sdl3_cmake")
        .from("sdl3")
        .configure("cmake -S @SRC@ -B @BUILD@ -GNinja")
        .build("cmake --build @BUILD@ --parallel")
        .env("CMAKE_COLOR_DIAGNOSTICS=ON")
        .define("CMAKE_BUILD_TYPE=Release",
                "SDL_STATIC=OFF",
                "SDL_SHARED=ON",
                "SDL_TEST_LIBRARY=OFF",
                "SDL_UNIX_CONSOLE_BUILD=ON")
        .produces_shared("libSDL3.so");

    p.exe("ex_08_sdl3", { "main.cpp" })
        .sys({ "../../../build/_deps/sdl3/src/include" })
        .dep("sdl3_cmake");
}
