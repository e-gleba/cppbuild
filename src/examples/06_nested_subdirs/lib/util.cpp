#include "util.hpp"
#include <cctype>
#include <print>
#include <string>

namespace util {
auto shout(std::string_view s) -> void
{
    std::string up(s);
    for (auto& c : up) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    std::println("[06_nested] {}!!!", up);
}
} // namespace util
