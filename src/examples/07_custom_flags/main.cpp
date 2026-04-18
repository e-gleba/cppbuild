#include <print>

#ifndef EX07_GREETING
#define EX07_GREETING "unset"
#endif

auto main() -> int
{
    std::println("[07_custom_flags] EX07_GREETING={}  NDEBUG={}",
                 EX07_GREETING,
#ifdef NDEBUG
                 "yes"
#else
                 "no"
#endif
    );
    return 0;
}
