#include "mathx.hpp"
#include <print>

auto main() -> int
{
    std::println("[02_static_lib] add(2,3)={}  mul(4,5)={}",
                 mathx::add(2, 3),
                 mathx::mul(4, 5));
    return 0;
}
