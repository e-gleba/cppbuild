#include "parts.hpp"
#include <print>

auto main() -> int
{
    std::println("[04_unity_build] sum = {}", part_a() + part_b() + part_c());
    return 0;
}
