// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/primitives.hxx — fixed-capacity consteval containers
//
//  Stepanov-ish: regular, minimal, generic. ct_string and ct_vector are the
//  only consteval-safe building blocks. Everything above is composed of them.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "errors.hxx"

#include <array>
#include <cstddef>
#include <source_location>
#include <string_view>

namespace build {

template <std::size_t Cap = 4096> struct ct_string final
{
    std::array<char, Cap> buf_{};
    std::size_t           len_{};

    constexpr ct_string() = default;

    // append is constexpr so the same emitters work at compile time and
    // at runtime (runtime expansion for glob/unity targets). Overflow is
    // an immediate compile error in consteval contexts and a runtime
    // abort-with-message otherwise.
    constexpr auto& append(std::string_view sv)
    {
        if (len_ + sv.size() >= Cap) {
            if consteval {
                consteval_fail("[ct_string] capacity exceeded — grow Cap");
            } else {
                die("ct_string capacity exceeded — grow Cap");
            }
        }
        for (std::size_t i = 0; i < sv.size(); ++i) {
            buf_[len_ + i] = sv[i];
        }
        len_ += sv.size();
        return *this;
    }

    constexpr auto& append(char c) { return append(std::string_view{ &c, 1 }); }
    constexpr auto& space() { return append(' '); }

    [[nodiscard]] constexpr auto view() const -> std::string_view
    {
        return { buf_.data(), len_ };
    }
    [[nodiscard]] constexpr auto c_str() const -> const char*
    {
        return buf_.data();
    }
    [[nodiscard]] constexpr auto size() const -> std::size_t { return len_; }
    [[nodiscard]] constexpr auto is_empty() const -> bool { return len_ == 0; }
    [[nodiscard]] constexpr auto contains(std::string_view s) const -> bool
    {
        return view().contains(s);
    }
    [[nodiscard]] constexpr auto starts_with(std::string_view s) const -> bool
    {
        return view().starts_with(s);
    }
};

template <typename T, std::size_t Cap> struct ct_vector final
{
    std::array<T, Cap> data_{};
    std::size_t        count_{};

    constexpr ct_vector() = default;

    constexpr void push_back(const T& v)
    {
        if (count_ >= Cap) {
            if consteval {
                consteval_fail("[ct_vector] capacity exceeded — grow Cap");
            } else {
                die("ct_vector capacity exceeded — grow Cap");
            }
        }
        data_[count_++] = v;
    }

    [[nodiscard]] constexpr auto& operator[](std::size_t i) { return data_[i]; }
    [[nodiscard]] constexpr auto& operator[](std::size_t i) const
    {
        return data_[i];
    }
    [[nodiscard]] constexpr auto size() const -> std::size_t { return count_; }
    [[nodiscard]] constexpr auto is_empty() const -> bool
    {
        return count_ == 0;
    }
    [[nodiscard]] constexpr auto begin() const { return data_.data(); }
    [[nodiscard]] constexpr auto end() const { return data_.data() + count_; }
};

using ct_path     = ct_string<256>;
using path_list   = ct_vector<ct_path, 64>;
using string_list = ct_vector<std::string_view, 64>;
constexpr auto npos = static_cast<std::size_t>(-1);

// Strip leading "./" and "/./" artifacts that compilers tack onto
// __FILE__ / source_location for relatively-included files. Keeps paths
// stable across builds and keeps object-file names tidy.
[[nodiscard]] consteval auto normalize_path(std::string_view p)
    -> std::string_view
{
    while (p.size() >= 2 && p[0] == '.' && p[1] == '/') {
        p.remove_prefix(2);
    }
    return p;
}

// Directory portion of a path (no trailing slash). "src/test/build.cxx" ->
// "src/test", "build.cxx" -> "".
[[nodiscard]] consteval auto dirname(std::string_view p) -> std::string_view
{
    p = normalize_path(p);
    auto slash = p.find_last_of('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    return p.substr(0, slash);
}

[[nodiscard]] consteval auto is_absolute(std::string_view p) -> bool
{
    return !p.empty() && p.front() == '/';
}

// Resolve `p` relative to directory `dir`. Absolute or empty-dir paths pass
// through. Output is written into a ct_path so it owns its storage (the
// stored thing lives in the project and must outlive string_views into it).
// "." and "./" as the relative path collapse to the dir itself.
[[nodiscard]] consteval auto resolve_under(std::string_view dir,
                                           std::string_view p) -> ct_path
{
    ct_path out;
    p = normalize_path(p);
    if (is_absolute(p) || dir.empty()) {
        out.append(p.empty() ? std::string_view{ "." } : p);
        return out;
    }
    if (p == "." || p.empty()) {
        out.append(dir);
        return out;
    }
    out.append(dir).append('/').append(p);
    return out;
}

// A string-literal-like path that captures std::source_location at the
// call site of its implicit conversion. Exists purely so users can write
// `add_sources("hello.cpp")` in any file and have it resolved against the
// directory of *that* file, not against the project root.
//
// The implicit ctor is the key: the default source_location argument is
// evaluated where the temporary is constructed — i.e. at the call site.
struct here_path final
{
    std::string_view raw{};
    std::string_view caller_dir{};

    consteval here_path(const char* s,
                        std::source_location loc =
                            std::source_location::current()) noexcept
        : raw(s)
        , caller_dir(dirname(loc.file_name()))
    {
    }

    [[nodiscard]] consteval auto resolved() const -> ct_path
    {
        return resolve_under(caller_dir, raw);
    }
};

// Deferred pattern: glob or regex. Captured at the user's call site so
// runtime expansion is anchored to the directory of the file that wrote
// the pattern, not the executor's CWD or the project root.
enum class pattern_kind : unsigned char
{
    glob,
    regex
};

struct here_pattern final
{
    std::string_view raw{};
    std::string_view caller_dir{};
    pattern_kind     kind{ pattern_kind::glob };

    consteval here_pattern(const char* s,
                           pattern_kind k,
                           std::source_location loc =
                               std::source_location::current()) noexcept
        : raw(s)
        , caller_dir(dirname(loc.file_name()))
        , kind(k)
    {
    }

    // Compact serialization to a single ct_string so we can store a list
    // of them in the target without pulling in heterogeneous types. Form:
    //   "<kind>|<caller_dir>|<raw>"   (kind = 'g' or 'r')
    [[nodiscard]] consteval auto serialized() const -> ct_string<512>
    {
        ct_string<512> out;
        out.append(kind == pattern_kind::glob ? 'g' : 'r');
        out.append('|').append(caller_dir);
        out.append('|').append(raw);
        return out;
    }
};

} // namespace build
