// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/expand.hxx — runtime expander for deferred targets.
//
//  A target is "deferred" when its source list isn't knowable at compile
//  time — i.e. it uses add_sources_glob / add_sources_regex, or
//  enable_unity_build. For those, we wait until the executor runs, then:
//
//    1. Materialize the final source list. Static sources (add_sources)
//       are kept; each pattern is expanded via std::filesystem plus a
//       glob-to-regex converter (or std::regex for the regex variant).
//       Results are made deterministic via std::sort.
//
//    2. If unity_build is on, write build/obj/<tgt>/unity.cpp once — a
//       generated file that #includes every resolved source — and emit
//       one compile step for it. Otherwise emit one compile step per
//       resolved source.
//
//    3. Emit the link/archive step using the resolved object list.
//
//    4. Emit compile_commands.json entries for every source (unity or
//       not) so clangd still sees per-file flags for the raw inputs.
//
//  The runtime emitters mirror the consteval ones in emit.hxx — same
//  flag order, same -I dedup, same -fPIC rules. They accept std::string
//  so we can paste in runtime-discovered file names without copying into
//  a ct_string.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "plan.hxx"
#include "project.hxx"
#include "runtime.hxx"
#include "target.hxx"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace build {

namespace expand_detail {

inline auto flat_object_name(std::string_view src) -> std::string
{
    std::string out;
    out.reserve(src.size() + 2);
    for (char c : src) {
        out.push_back((c == '/' || c == '\\') ? '_' : c);
    }
    out += ".o";
    return out;
}

inline auto object_path(std::string_view build_dir,
                        std::string_view tgt,
                        std::string_view src) -> std::string
{
    std::string p;
    p.reserve(build_dir.size() + tgt.size() + src.size() + 16);
    p += build_dir;
    p += "/obj/";
    p += tgt;
    p += '/';
    p += flat_object_name(src);
    return p;
}

inline auto depfile_path(std::string_view build_dir,
                         std::string_view tgt,
                         std::string_view src) -> std::string
{
    return object_path(build_dir, tgt, src) + ".d";
}

// Convert a filesystem glob ("*.cpp", "**/*.cpp") to an ECMAScript regex.
// Supports `*`, `?`, `**` and literal characters. Glob anchored to whole
// path. Directory separator is '/'.
inline auto glob_to_regex(std::string_view glob) -> std::regex
{
    std::string re = "^";
    for (std::size_t i = 0; i < glob.size(); ++i) {
        char c = glob[i];
        if (c == '*') {
            if (i + 1 < glob.size() && glob[i + 1] == '*') {
                re += ".*";
                ++i;
            } else {
                re += "[^/]*";
            }
        } else if (c == '?') {
            re += "[^/]";
        } else if (c == '.' || c == '+' || c == '(' || c == ')' || c == '|' ||
                   c == '^' || c == '$' || c == '{' || c == '}' || c == '[' ||
                   c == ']' || c == '\\') {
            re += '\\';
            re += c;
        } else {
            re += c;
        }
    }
    re += '$';
    return std::regex(re, std::regex::ECMAScript);
}

// Expand a here_pattern-serialized blob. Form: "<kind>|<caller_dir>|<raw>".
// Walks caller_dir recursively, pushing every regular file whose
// caller_dir-relative path matches the pattern. Returns absolute-ish
// paths relative to project root (same kind of strings a user would have
// written into add_sources).
inline auto expand_pattern(std::string_view serialized)
    -> std::vector<std::string>
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;

    if (serialized.size() < 4) {
        return out;
    }
    char kind = serialized[0];
    auto rest = serialized.substr(2); // skip "<kind>|"
    auto bar  = rest.find('|');
    if (bar == std::string_view::npos) {
        return out;
    }
    std::string_view caller_dir = rest.substr(0, bar);
    std::string_view raw        = rest.substr(bar + 1);

    fs::path root = caller_dir.empty() ? fs::path(".") : fs::path(caller_dir);
    if (!fs::exists(root)) {
        return out;
    }

    std::regex re = (kind == 'r') ? std::regex(std::string(raw),
                                               std::regex::ECMAScript)
                                  : glob_to_regex(raw);

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (!it->is_regular_file()) {
            continue;
        }
        auto rel = fs::relative(it->path(), root, ec).generic_string();
        if (ec) {
            continue;
        }
        if (std::regex_match(rel, re)) {
            std::string final_path;
            if (caller_dir.empty()) {
                final_path = rel;
            } else {
                final_path.reserve(caller_dir.size() + 1 + rel.size());
                final_path.append(caller_dir);
                final_path.push_back('/');
                final_path.append(rel);
            }
            out.push_back(std::move(final_path));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Resolve the full source list for a target (static + globbed/regex).
inline auto resolve_sources(const target& t) -> std::vector<std::string>
{
    std::vector<std::string> sources;
    sources.reserve(t.sources_.size() + 16);
    for (const auto& p : t.sources_) {
        sources.emplace_back(p.c_str(), p.size());
    }
    for (const auto& pat : t.glob_patterns_) {
        auto matches = expand_pattern(pat.view());
        for (auto& m : matches) {
            sources.push_back(std::move(m));
        }
    }
    // Deterministic, deduped.
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
}

inline auto std_flag(standard s) -> std::string_view
{
    switch (s) {
        case standard::cpp17: return "-std=c++17";
        case standard::cpp20: return "-std=c++20";
        case standard::cpp23: return "-std=c++23";
        case standard::cpp26: return "-std=c++26";
    }
    return "-std=c++23";
}

inline auto compiler_flag(compiler_id c) -> std::string_view
{
    switch (c) {
        case compiler_id::clang: return "clang++";
        case compiler_id::gcc:   return "g++";
    }
    return "clang++";
}

struct opt_defs
{
    std::string_view opt, def;
};
inline auto opt_for(build_type b) -> opt_defs
{
    switch (b) {
        case build_type::debug:              return { "-O0 -g", "" };
        case build_type::release:            return { "-O2", "-DNDEBUG" };
        case build_type::release_debug_info: return { "-O2 -g", "-DNDEBUG" };
        case build_type::min_size:           return { "-Os", "-DNDEBUG" };
    }
    return { "-O2", "-DNDEBUG" };
}

inline auto common_flags(const project& proj, const target& tgt) -> std::string
{
    std::string s;
    s += compiler_flag(proj.compiler_);
    s += ' ';
    s += std_flag(tgt.standard_);
    auto [opt, def] = opt_for(tgt.build_type_);
    s += ' ';
    s += opt;
    if (!def.empty()) {
        s += ' ';
        s += def;
    }
    if (tgt.wall_)     { s += " -Wall"; }
    if (tgt.wextra_)   { s += " -Wextra"; }
    if (tgt.pedantic_) { s += " -Wpedantic"; }
    if (tgt.werror_)   { s += " -Werror"; }
    for (auto f : proj.global_flags_) {
        s += ' ';
        s += f;
    }
    for (auto d : proj.global_definitions_) {
        s += " -D";
        s += d;
    }
    for (auto f : tgt.compiler_flags_) {
        s += ' ';
        s += f;
    }
    for (auto d : tgt.definitions_) {
        s += " -D";
        s += d;
    }
    std::vector<std::string> seen;
    auto add_inc = [&](std::string_view p, std::string_view flag) {
        for (const auto& prev : seen) {
            if (prev == p) {
                return;
            }
        }
        seen.emplace_back(p);
        s += ' ';
        s += flag;
        s += ' ';
        s += p;
    };
    for (const auto& d : proj.global_system_include_dirs_) {
        add_inc(d.view(), "-isystem");
    }
    for (const auto& d : tgt.system_include_dirs_) {
        add_inc(d.view(), "-isystem");
    }
    for (auto dep_name : tgt.dependencies_) {
        if (const auto* dep = proj.find_target(dep_name)) {
            for (const auto& d : dep->system_include_dirs_) {
                add_inc(d.view(), "-isystem");
            }
        }
    }
    if (!proj.generated_.is_empty()) {
        std::string gen;
        gen.append(proj.build_dir_);
        gen.append("/generated");
        add_inc(gen, "-I");
    }
    for (const auto& d : proj.global_include_dirs_) {
        add_inc(d.view(), "-I");
    }
    for (const auto& d : tgt.include_dirs_) {
        add_inc(d.view(), "-I");
    }
    for (auto dep_name : tgt.dependencies_) {
        if (const auto* dep = proj.find_target(dep_name)) {
            for (const auto& d : dep->include_dirs_) {
                add_inc(d.view(), "-I");
            }
        }
    }
    return s;
}

} // namespace expand_detail

// Expand one deferred target into its portion of the plan. Appends to
// `steps` and `db_entries` in place, matching the format make_plan uses
// for static targets so the executor doesn't need to care which is which.
inline void expand_deferred_target(const project& proj,
                                   const target&  tgt,
                                   std::vector<std::string>& final_sources_out,
                                   std::vector<build_step>&  steps_out,
                                   std::vector<compile_db_entry>& db_out)
{
    namespace fs = std::filesystem;
    using namespace expand_detail;

    auto sources = resolve_sources(tgt);
    if (sources.empty()) {
        // Not necessarily an error at this layer — but the executor will
        // flag it with a clear diagnostic.
        return;
    }

    const std::string_view bdir  = proj.build_dir_;
    const std::string_view tname = tgt.name_;

    auto append_step = [&](std::string_view command,
                           std::string_view output,
                           std::string_view depfile,
                           std::string_view primary,
                           bool             is_unity) {
        build_step st;
        st.command.append(command);
        st.output_path.append(output);
        st.depfile_path.append(depfile);
        st.primary_input.append(primary);
        st.label      = tname;
        st.is_compile = true;
        st.is_unity   = is_unity;
        steps_out.push_back(st);
    };

    const std::string pic_flag =
        (tgt.kind_ == target_kind::shared_library) ? " -fPIC" : "";

    if (tgt.unity_) {
        // One synthetic .cpp that includes every resolved source.
        std::string unity_src =
            std::string(bdir) + "/obj/" + std::string(tname) + "/unity.cpp";
        std::string obj     = object_path(bdir, tname, unity_src);
        std::string dep     = depfile_path(bdir, tname, unity_src);
        std::string cmd     = common_flags(proj, tgt);
        cmd += pic_flag;
        cmd += " -MD -MF ";
        cmd += dep;
        cmd += " -c ";
        cmd += unity_src;
        cmd += " -o ";
        cmd += obj;
        append_step(std::move(cmd),
                    std::move(obj),
                    std::move(dep),
                    unity_src,
                    /*is_unity=*/true);
    } else {
        for (const auto& src : sources) {
            std::string obj = object_path(bdir, tname, src);
            std::string dep = depfile_path(bdir, tname, src);
            std::string cmd = common_flags(proj, tgt);
            cmd += pic_flag;
            cmd += " -MD -MF ";
            cmd += dep;
            cmd += " -c ";
            cmd += src;
            cmd += " -o ";
            cmd += obj;
            append_step(std::move(cmd),
                        std::move(obj),
                        std::move(dep),
                        src,
                        /*is_unity=*/false);
        }
    }

    // Link / archive.
    std::string link_out;
    std::string link_cmd;
    if (tgt.kind_ == target_kind::executable) {
        link_out = std::string(bdir) + '/' + std::string(tname);
        link_cmd = common_flags(proj, tgt);
        if (tgt.unity_) {
            link_cmd += ' ';
            link_cmd += object_path(bdir,
                                    tname,
                                    std::string(bdir) + "/obj/" +
                                        std::string(tname) + "/unity.cpp");
        } else {
            for (const auto& src : sources) {
                link_cmd += ' ';
                link_cmd += object_path(bdir, tname, src);
            }
        }
        for (auto dep_name : tgt.dependencies_) {
            if (const auto* dep = proj.find_target(dep_name)) {
                if (dep->kind_ == target_kind::static_library) {
                    link_cmd += ' ';
                    link_cmd += std::string(bdir) + "/lib" +
                                std::string(dep_name) + ".a";
                } else if (dep->kind_ == target_kind::shared_library) {
                    link_cmd += " -L";
                    link_cmd += bdir;
                    link_cmd += " -l";
                    link_cmd += dep_name;
                    link_cmd += " -Wl,-rpath,";
                    link_cmd += bdir;
                }
            } else if (const auto* ext = proj.find_external(dep_name)) {
                if (ext->kind == external_kind::shared_library ||
                    ext->kind == external_kind::static_library) {
                    std::string edir =
                        std::string(bdir) + "/_deps/" +
                        std::string(ext->name) + "/build";
                    link_cmd += ' ';
                    link_cmd += edir + '/' + std::string(ext->produces_file);
                    if (ext->kind == external_kind::shared_library) {
                        link_cmd += " -Wl,-rpath,";
                        link_cmd += edir;
                    }
                }
            }
        }
        for (auto l : tgt.linker_flags_) {
            link_cmd += ' ';
            link_cmd += l;
        }
        link_cmd += " -o ";
        link_cmd += link_out;
    } else if (tgt.kind_ == target_kind::shared_library) {
        link_out = std::string(bdir) + "/lib" + std::string(tname) + ".so";
        link_cmd = common_flags(proj, tgt);
        link_cmd += " -fPIC -shared";
        if (!tgt.soname_.empty()) {
            link_cmd += " -Wl,-soname,";
            link_cmd += tgt.soname_;
        }
        if (tgt.unity_) {
            link_cmd += ' ';
            link_cmd += object_path(bdir,
                                    tname,
                                    std::string(bdir) + "/obj/" +
                                        std::string(tname) + "/unity.cpp");
        } else {
            for (const auto& src : sources) {
                link_cmd += ' ';
                link_cmd += object_path(bdir, tname, src);
            }
        }
        for (auto dep_name : tgt.dependencies_) {
            if (const auto* dep = proj.find_target(dep_name)) {
                if (dep->kind_ == target_kind::static_library) {
                    for (const auto& s : dep->sources_) {
                        link_cmd += ' ';
                        link_cmd +=
                            object_path(bdir, dep->name_, s.view());
                    }
                } else if (dep->kind_ == target_kind::shared_library) {
                    link_cmd += " -L";
                    link_cmd += bdir;
                    link_cmd += " -l";
                    link_cmd += dep_name;
                    link_cmd += " -Wl,-rpath,";
                    link_cmd += bdir;
                }
            }
        }
        for (auto l : tgt.linker_flags_) {
            link_cmd += ' ';
            link_cmd += l;
        }
        link_cmd += " -o ";
        link_cmd += link_out;
    } else /* static_library */ {
        link_out = std::string(bdir) + "/lib" + std::string(tname) + ".a";
        link_cmd = "ar rcs ";
        link_cmd += link_out;
        if (tgt.unity_) {
            link_cmd += ' ';
            link_cmd += object_path(bdir,
                                    tname,
                                    std::string(bdir) + "/obj/" +
                                        std::string(tname) + "/unity.cpp");
        } else {
            for (const auto& src : sources) {
                link_cmd += ' ';
                link_cmd += object_path(bdir, tname, src);
            }
        }
    }

    build_step link_step;
    link_step.command.append(link_cmd);
    link_step.output_path.append(link_out);
    link_step.label = tname;
    link_step.is_compile = false;
    steps_out.push_back(link_step);

    // compile_commands.json entries: one per *real* source, so clangd
    // indexes actual files. For unity builds we still list each source
    // individually (the unity.cpp exists only to speed up compilation).
    for (const auto& src : sources) {
        std::string cmd = common_flags(proj, tgt);
        cmd += pic_flag;
        cmd += " -c ";
        cmd += src;
        compile_db_entry e;
        e.command.append(cmd);
        e.file.append(src);
        db_out.push_back(e);
    }

    final_sources_out = std::move(sources);
}

// Write a unity.cpp for `tgt` at build_dir/obj/<tgt>/unity.cpp. The file
// #includes every real source (in sorted order). Only rewrites when the
// content actually changed so incremental stays correct.
inline auto write_unity_file(std::string_view bdir,
                             std::string_view tname,
                             const std::vector<std::string>& sources) -> std::string
{
    namespace fs = std::filesystem;
    fs::path p = fs::path(std::string(bdir)) / "obj" /
                 std::string(tname) / "unity.cpp";
    fs::create_directories(p.parent_path());

    std::string desired =
        "// Auto-generated unity file — do not edit.\n"
        "// Target: ";
    desired += tname;
    desired += '\n';
    for (const auto& s : sources) {
        auto abs = fs::absolute(fs::path(s)).string();
        desired += "#include \"";
        desired += abs;
        desired += "\"\n";
    }

    // Avoid touching mtime when content didn't change → keeps cache warm.
    (void) write_if_different(p, desired);
    return p.string();
}

} // namespace build
