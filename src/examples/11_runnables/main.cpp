#include <print>
#include <string_view>

auto main(int argc, char** argv) -> int
{
    std::string_view mode = argc > 1 ? argv[1] : "run";
    if (mode == "--smoke") {
        std::println("[11_runnables] smoke OK");
        return 0;
    }
    std::println("[11_runnables] hello from runnables example");
    return 0;
}
