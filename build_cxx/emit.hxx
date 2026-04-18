// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/emit.hxx — command-string generation.
//
//  Model:
//    - every source -> one .o compile command (emit_object_command) that
//      also writes a Make-style depfile via -MD -MF for header tracking
//    - executables  -> link .o files of self + deps' .a + shared -L/-l
//    - static libs  -> ar rcs libX.a X.o ...
//    - shared libs  -> -shared -fPIC self.o + static deps' .o -o libX.so
//
//  Object paths live under <build_dir>/obj/<target>/<flattened-src>.o so
//  two targets compiling the same source with different flags never
//  collide. Depfiles live next to the object: <obj>.d.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "config.hxx"
#include "primitives.hxx"
#include "project.hxx"
#include "target.hxx"

#include <string_view>

namespace build {

// Flatten "src/test/hello.cpp" -> "src_test_hello.cpp.o".
consteval void append_flat_name(ct_string<256>& out, std::string_view src)
{
    for (char c : src) {
        if (c == '/' || c == '\\') {
            out.append('_');
        } else {
            out.append(c);
        }
    }
    out.append(".o");
}

consteval void append_object_path(ct_string<512>&  out,
                                  std::string_view build_dir,
                                  std::string_view target_name,
                                  std::string_view src)
{
    out.append(build_dir).append("/obj/").append(target_name).append('/');
    ct_string<256> flat;
    append_flat_name(flat, src);
    out.append(flat.view());
}

consteval void append_depfile_path(ct_string<512>&  out,
                                   std::string_view build_dir,
                                   std::string_view target_name,
                                   std::string_view src)
{
    append_object_path(out, build_dir, target_name, src);
    out.append(".d");
}

consteval void emit_common_flags(ct_string<4096>& cmd,
                                 const project&   proj,
                                 const target&    tgt)
{
    cmd.append(to_flag(proj.compiler_)).space().append(to_flag(tgt.standard_));
    auto [opt, def] = to_flags(tgt.build_type_);
    cmd.space().append(opt);
    if (!def.empty()) {
        cmd.space().append(def);
    }
    if (tgt.wall_) {
        cmd.space().append("-Wall");
    }
    if (tgt.wextra_) {
        cmd.space().append("-Wextra");
    }
    if (tgt.pedantic_) {
        cmd.space().append("-Wpedantic");
    }
    if (tgt.werror_) {
        cmd.space().append("-Werror");
    }
    for (auto f : proj.global_flags_) {
        cmd.space().append(f);
    }
    for (auto d : proj.global_definitions_) {
        cmd.space().append("-D").append(d);
    }
    for (auto f : tgt.compiler_flags_) {
        cmd.space().append(f);
    }
    for (auto d : tgt.definitions_) {
        cmd.space().append("-D").append(d);
    }
    // Dedupe include dirs: the same path often appears explicitly on a
    // consumer and transitively via a library dep. Skipping duplicates
    // keeps the command short and keeps compile_commands.json stable.
    // System dirs win when a path appears in both sets (quieter).
    path_list seen;
    auto append_unique = [&](std::string_view p, std::string_view flag) {
        for (const auto& prev : seen) {
            if (prev.view() == p) {
                return;
            }
        }
        ct_path copy;
        copy.append(p);
        seen.push_back(copy);
        cmd.space().append(flag).space().append(p);
    };
    for (const auto& d : proj.global_system_include_dirs_) {
        append_unique(d.view(), "-isystem");
    }
    for (const auto& d : tgt.system_include_dirs_) {
        append_unique(d.view(), "-isystem");
    }
    for (auto dep_name : tgt.dependencies_) {
        if (const auto* dep = proj.find_target(dep_name)) {
            for (const auto& d : dep->system_include_dirs_) {
                append_unique(d.view(), "-isystem");
            }
        }
    }
    // Auto-injected: <build_dir>/generated when the project declares
    // generate_file(...). Users #include the generated headers by bare
    // name without per-target -I boilerplate.
    if (!proj.generated_.is_empty()) {
        ct_string<512> gen;
        gen.append(proj.build_dir_).append("/generated");
        append_unique(gen.view(), "-I");
    }
    for (const auto& d : proj.global_include_dirs_) {
        append_unique(d.view(), "-I");
    }
    for (const auto& d : tgt.include_dirs_) {
        append_unique(d.view(), "-I");
    }
    for (auto dep_name : tgt.dependencies_) {
        if (const auto* dep = proj.find_target(dep_name)) {
            for (const auto& d : dep->include_dirs_) {
                append_unique(d.view(), "-I");
            }
        }
    }
}

// Per-source compile command with depfile:
//   <compiler> <flags> [-fPIC] -MD -MF <obj>.d -c <src> -o <obj>
[[nodiscard]] consteval auto emit_object_command(const project&   proj,
                                                 const target&    tgt,
                                                 std::string_view src)
    -> ct_string<4096>
{
    ct_string<4096> cmd;
    emit_common_flags(cmd, proj, tgt);
    if (tgt.kind_ == target_kind::shared_library) {
        cmd.space().append("-fPIC");
    }
    ct_string<512> obj;
    append_object_path(obj, proj.build_dir_, tgt.name_, src);
    ct_string<512> dep;
    append_depfile_path(dep, proj.build_dir_, tgt.name_, src);
    cmd.space().append("-MD").space().append("-MF").space().append(dep.view());
    cmd.space().append("-c").space().append(src);
    cmd.space().append("-o").space().append(obj.view());
    return cmd;
}

// compile_commands.json entry: command without -o / -MD / -MF. clangd
// doesn't need them and keeping them out keeps the db stable.
[[nodiscard]] consteval auto emit_file_command(const project&   proj,
                                               const target&    tgt,
                                               std::string_view src)
    -> ct_string<4096>
{
    ct_string<4096> cmd;
    emit_common_flags(cmd, proj, tgt);
    if (tgt.kind_ == target_kind::shared_library) {
        cmd.space().append("-fPIC");
    }
    cmd.space().append("-c").space().append(src);
    return cmd;
}

// ar rcs libX.a obj1.o obj2.o ...
[[nodiscard]] consteval auto emit_static_archive_command(const project& proj,
                                                         const target& tgt)
    -> ct_string<4096>
{
    ct_string<4096> cmd;
    cmd.append("ar rcs ")
        .append(proj.build_dir_)
        .append("/lib")
        .append(tgt.name_)
        .append(to_output_ext(tgt.kind_));
    for (const auto& s : tgt.sources_) {
        cmd.space();
        ct_string<512> obj;
        append_object_path(obj, proj.build_dir_, tgt.name_, s.view());
        cmd.append(obj.view());
    }
    return cmd;
}

// <compiler> <flags> -shared -fPIC [-Wl,-soname,X] <self.o...>
//   <static_deps.o...> <linker_flags> -o <build_dir>/libX.so
[[nodiscard]] consteval auto emit_shared_link_command(const project& proj,
                                                      const target&  tgt)
    -> ct_string<4096>
{
    ct_string<4096> cmd;
    emit_common_flags(cmd, proj, tgt);
    cmd.space().append("-fPIC").space().append("-shared");
    if (!tgt.soname_.empty()) {
        cmd.space().append("-Wl,-soname,").append(tgt.soname_);
    }
    for (const auto& s : tgt.sources_) {
        cmd.space();
        ct_string<512> obj;
        append_object_path(obj, proj.build_dir_, tgt.name_, s.view());
        cmd.append(obj.view());
    }
    for (auto dep_name : tgt.dependencies_) {
        if (const auto* dep = proj.find_target(dep_name)) {
            if (dep->kind_ == target_kind::static_library) {
                for (const auto& s : dep->sources_) {
                    cmd.space();
                    ct_string<512> obj;
                    append_object_path(obj,
                                       proj.build_dir_,
                                       dep->name_,
                                       s.view());
                    cmd.append(obj.view());
                }
            } else if (dep->kind_ == target_kind::shared_library) {
                cmd.space().append("-L").append(proj.build_dir_);
                cmd.space().append("-l").append(dep_name);
                cmd.space().append("-Wl,-rpath,").append(proj.build_dir_);
            }
        } else if (const auto* ext = proj.find_external(dep_name)) {
            if (ext->kind == external_kind::shared_library ||
                ext->kind == external_kind::static_library) {
                cmd.space()
                    .append(proj.build_dir_)
                    .append("/_deps/")
                    .append(ext->name)
                    .append("/build/")
                    .append(ext->produces_file);
                if (ext->kind == external_kind::shared_library) {
                    cmd.space()
                        .append("-Wl,-rpath,")
                        .append(proj.build_dir_)
                        .append("/_deps/")
                        .append(ext->name)
                        .append("/build");
                }
            }
        }
    }
    for (auto l : tgt.linker_flags_) {
        cmd.space().append(l);
    }
    cmd.space()
        .append("-o")
        .space()
        .append(proj.build_dir_)
        .append('/')
        .append("lib")
        .append(tgt.name_)
        .append(to_output_ext(tgt.kind_));
    return cmd;
}

// Executable link: self.o + static deps via libX.a + shared deps via -L/-l.
[[nodiscard]] consteval auto emit_link_command(const project& proj,
                                               const target&  tgt)
    -> ct_string<4096>
{
    ct_string<4096> cmd;
    emit_common_flags(cmd, proj, tgt);
    for (const auto& s : tgt.sources_) {
        cmd.space();
        ct_string<512> obj;
        append_object_path(obj, proj.build_dir_, tgt.name_, s.view());
        cmd.append(obj.view());
    }
    for (auto dep_name : tgt.dependencies_) {
        if (const auto* dep = proj.find_target(dep_name)) {
            if (dep->kind_ == target_kind::static_library) {
                cmd.space()
                    .append(proj.build_dir_)
                    .append("/lib")
                    .append(dep_name)
                    .append(".a");
            } else if (dep->kind_ == target_kind::shared_library) {
                cmd.space().append("-L").append(proj.build_dir_);
                cmd.space().append("-l").append(dep_name);
                cmd.space().append("-Wl,-rpath,").append(proj.build_dir_);
            }
        } else if (const auto* ext = proj.find_external(dep_name)) {
            if (ext->kind == external_kind::shared_library ||
                ext->kind == external_kind::static_library) {
                cmd.space()
                    .append(proj.build_dir_)
                    .append("/_deps/")
                    .append(ext->name)
                    .append("/build/")
                    .append(ext->produces_file);
                if (ext->kind == external_kind::shared_library) {
                    cmd.space()
                        .append("-Wl,-rpath,")
                        .append(proj.build_dir_)
                        .append("/_deps/")
                        .append(ext->name)
                        .append("/build");
                }
            }
        }
    }
    for (auto l : tgt.linker_flags_) {
        cmd.space().append(l);
    }
    cmd.space()
        .append("-o")
        .space()
        .append(proj.build_dir_)
        .append('/')
        .append(tgt.name_)
        .append(to_output_ext(tgt.kind_));
    return cmd;
}

} // namespace build
