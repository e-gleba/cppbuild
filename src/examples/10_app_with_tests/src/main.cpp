#include "calc/calc.hpp"

#include <print>

#include "ex10/version.hpp"

int main()
{
    std::println("{} v{}", ex10::project, ex10::version);
    std::println("add(2,3)      = {}", calc::add(2, 3));
    std::println("mul(4,5)      = {}", calc::mul(4, 5));
    std::println("factorial(10) = {}", calc::factorial(10));
    return 0;
}
