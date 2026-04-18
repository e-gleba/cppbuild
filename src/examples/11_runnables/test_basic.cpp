#include <print>

auto main() -> int
{
    int a = 2, b = 3;
    if (a + b != 5) {
        std::println(stderr, "[11_runnables/test] FAIL: 2 + 3 != 5");
        return 1;
    }
    std::println("[11_runnables/test] PASS");
    return 0;
}
