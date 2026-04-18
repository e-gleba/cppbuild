#pragma once

namespace calc {

[[nodiscard]] auto add(int a, int b) noexcept -> int;
[[nodiscard]] auto mul(int a, int b) noexcept -> int;
[[nodiscard]] auto factorial(int n) noexcept -> long long;

} // namespace calc
