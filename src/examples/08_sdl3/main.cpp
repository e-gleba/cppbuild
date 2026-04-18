// Minimal SDL3 smoke test: initialise the subsystem, print the version
// the build linked against, shut down. Runs headless — no window, no
// display required.
#include <SDL3/SDL.h>
#include <cstdio>

auto main() -> int
{
    if (!SDL_Init(0)) {
        std::fprintf(stderr, "[08_sdl3] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int v = SDL_GetVersion();
    std::printf("[08_sdl3] SDL3 linked at runtime: %d.%d.%d (revision %s)\n",
                SDL_VERSIONNUM_MAJOR(v),
                SDL_VERSIONNUM_MINOR(v),
                SDL_VERSIONNUM_MICRO(v),
                SDL_GetRevision());

    SDL_Quit();
    return 0;
}
