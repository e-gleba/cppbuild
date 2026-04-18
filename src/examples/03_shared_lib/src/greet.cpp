#include "greet.hpp"
#include <print>

namespace greet {
auto hello(std::string_view name) -> void
{
    std::println("[03_shared_lib] hello, {}!", name);
}
} // namespace greet
