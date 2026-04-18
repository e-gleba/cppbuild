#include "calc/calc.hpp"

namespace calc {

auto add(int a, int b) noexcept -> int { return a + b; }
auto mul(int a, int b) noexcept -> int { return a * b; }

auto factorial(int n) noexcept -> long long
{
    long long r = 1;
    for (int i = 2; i <= n; ++i) {
        r *= i;
    }
    return r;
}

} // namespace calc
