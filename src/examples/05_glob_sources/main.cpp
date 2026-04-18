#include "all.hpp"
#include <print>

auto main() -> int
{
    std::println("[05_glob_sources] total = {}", one() + two() + three());
    return 0;
}
