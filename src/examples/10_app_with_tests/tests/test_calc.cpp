#include "calc/calc.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool cond, const char* expr, int line)
{
    if (!cond) {
        std::fprintf(stderr, "  FAIL (line %d): %s\n", line, expr);
        ++failures;
    }
}

} // namespace

#define CHECK(x) check((x), #x, __LINE__)

int main()
{
    CHECK(calc::add(2, 3) == 5);
    CHECK(calc::add(-1, 1) == 0);
    CHECK(calc::mul(4, 5) == 20);
    CHECK(calc::mul(0, 99) == 0);
    CHECK(calc::factorial(0) == 1);
    CHECK(calc::factorial(5) == 120);
    CHECK(calc::factorial(10) == 3628800);

    if (failures == 0) {
        std::printf("[test_calc] all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "[test_calc] %d failure(s)\n", failures);
    return 1;
}
