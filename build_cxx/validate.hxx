// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/validate.hxx — consteval validation. Every failure is routed
//  through consteval_fail, producing a compiler diagnostic that quotes
//  both a category and a specific message.
//
//  Validation runs unconditionally from make_plan(); an invalid project
//  never reaches the executor.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "errors.hxx"
#include "project.hxx"

namespace build {

consteval void validate(const project& p)
{
    if (p.name_.empty()) {
        consteval_fail("[project] name is empty");
    }
    if (p.build_dir_.empty()) {
        consteval_fail("[project] build_dir is empty");
    }
    if (p.targets_.is_empty()) {
        consteval_fail("[project] at least one target is required");
    }
    if (p.version_.major < 0 || p.version_.minor < 0 ||
        p.version_.patch < 0) {
        consteval_fail("[project] negative version component");
    }
    for (const auto& d : p.global_include_dirs_) {
        if (d.is_empty()) {
            consteval_fail("[project] empty include dir");
        }
    }
    for (const auto& d : p.global_system_include_dirs_) {
        if (d.is_empty()) {
            consteval_fail("[project] empty system include dir");
        }
    }
    for (std::size_t i = 0; i < p.generated_.size(); ++i) {
        const auto& g = p.generated_[i];
        if (g.rel_path.is_empty()) {
            consteval_fail("[generate_file] empty path");
        }
        // reject absolute paths — file lives under build/generated
        if (g.rel_path.view().front() == '/') {
            consteval_fail(
                "[generate_file] path must be relative to <build>/generated");
        }
        if (!g.is_dynamic() && g.literal_content.is_empty()) {
            consteval_fail("[generate_file] empty content");
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (p.generated_[k].rel_path.view() == g.rel_path.view()) {
                consteval_fail("[generate_file] duplicate path");
            }
        }
    }

    for (std::size_t i = 0; i < p.targets_.size(); ++i) {
        const auto& t = p.targets_[i];
        if (t.name_.empty()) {
            consteval_fail("[target] name is empty");
        }
        // Sources may be empty when at least one glob pattern is set —
        // the pattern will be expanded at runtime and yield sources.
        if (t.sources_.is_empty() && t.glob_patterns_.is_empty()) {
            consteval_fail("[target] no sources and no glob patterns");
        }
        for (const auto& pth : t.sources_) {
            if (pth.is_empty()) {
                consteval_fail("[target] empty source path");
            }
        }
        for (const auto& pth : t.include_dirs_) {
            if (pth.is_empty()) {
                consteval_fail("[target] empty include dir");
            }
        }
        for (const auto& pth : t.system_include_dirs_) {
            if (pth.is_empty()) {
                consteval_fail("[target] empty system include dir");
            }
        }
        for (auto sv : t.definitions_) {
            if (sv.empty()) {
                consteval_fail("[target] empty definition");
            }
        }
        for (auto sv : t.compiler_flags_) {
            if (sv.empty()) {
                consteval_fail("[target] empty compiler flag");
            }
        }
        for (auto sv : t.linker_flags_) {
            if (sv.empty()) {
                consteval_fail("[target] empty linker flag");
            }
        }
        for (const auto& pe : t.pre_build_) {
            if (pe.fn == nullptr && pe.literal.empty()) {
                consteval_fail("[target] empty .before() command");
            }
        }
        for (const auto& pe : t.post_build_) {
            if (pe.fn == nullptr && pe.literal.empty()) {
                consteval_fail("[target] empty .after() command");
            }
        }
        for (auto dep : t.dependencies_) {
            if (dep.empty()) {
                consteval_fail("[target] empty dependency name");
            }
            if (!p.find_target(dep) && !p.find_external(dep)) {
                consteval_fail("[target] unknown dependency — "
                               "declare the library or external before "
                               "its consumer");
            }
            if (dep == t.name_) {
                consteval_fail("[target] self-dependency is forbidden");
            }
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (p.targets_[k].name_ == t.name_) {
                consteval_fail("[project] duplicate target name");
            }
        }
    }

    // Fetches
    for (std::size_t i = 0; i < p.fetches_.size(); ++i) {
        const auto& f = p.fetches_[i];
        if (f.name.empty()) {
            consteval_fail("[fetch_git] name is empty");
        }
        if (f.url.empty()) {
            consteval_fail("[fetch_git] url is empty");
        }
        if (f.rev.empty()) {
            consteval_fail("[fetch_git] rev is empty — pin a tag / commit");
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (p.fetches_[k].name == f.name) {
                consteval_fail("[fetch_git] duplicate name");
            }
        }
    }

    // External projects
    for (std::size_t i = 0; i < p.externals_.size(); ++i) {
        const auto& e = p.externals_[i];
        if (e.name.empty()) {
            consteval_fail("[external_project] name is empty");
        }
        if (p.find_target(e.name)) {
            consteval_fail("[external_project] name collides with a target");
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (p.externals_[k].name == e.name) {
                consteval_fail("[external_project] duplicate name");
            }
        }
        if (!e.source_name.empty()) {
            bool found = false;
            for (const auto& f : p.fetches_) {
                if (f.name == e.source_name) { found = true; break; }
            }
            if (!found) {
                consteval_fail("[external_project] from() references an "
                               "unknown fetch_git name");
            }
        }
        // If a configure/build is given, we expect produces to be set so
        // consumers can link. Pure "run a side effect" externals without
        // a link product are allowed (kind::none).
        if (e.kind != external_kind::none && e.produces_file.empty()) {
            consteval_fail("[external_project] produces_* not set");
        }
        for (const auto& kv : e.env_vars) {
            if (kv.empty()) {
                consteval_fail("[external_project] empty env entry");
            }
            if (kv.find('=') == std::string_view::npos) {
                consteval_fail(
                    "[external_project] env entry must be KEY=VALUE");
            }
        }
        for (const auto& kv : e.defines) {
            if (kv.empty()) {
                consteval_fail("[external_project] empty define entry");
            }
            if (kv.find('=') == std::string_view::npos) {
                consteval_fail(
                    "[external_project] define entry must be KEY=VALUE");
            }
        }
    }

    // Runnables — named shell-command groups invoked via --run.
    for (std::size_t i = 0; i < p.runnables_.size(); ++i) {
        const auto& r = p.runnables_[i];
        if (r.name.empty()) {
            consteval_fail("[runnable] name is empty");
        }
        if (r.cmds.is_empty()) {
            consteval_fail("[runnable] must have at least one .cmd(...)");
        }
        for (const auto& c : r.cmds) {
            if (!c.is_dynamic() && c.literal.empty()) {
                consteval_fail("[runnable] empty command string");
            }
        }
        for (auto d : r.deps) {
            if (d.empty()) {
                consteval_fail("[runnable] empty dep name");
            }
            // Runnable deps must resolve to known targets at compile
            // time — typo here is the exact case that kills CMake users
            // at build time. We catch it in the compiler.
            if (!p.find_target(d) && !p.find_external(d)) {
                consteval_fail("[runnable] unknown dep — "
                               "declare the target before the runnable "
                               "that depends on it");
            }
        }
        // Runnables live in their own CLI verb (--run) so names may
        // intentionally alias targets (e.g. a runnable "test" that runs
        // the test target). Only same-verb collisions are flagged.
        for (std::size_t k = 0; k < i; ++k) {
            if (p.runnables_[k].name == r.name) {
                consteval_fail("[runnable] duplicate name");
            }
        }
    }

    // Project-level lifecycle hooks.
    for (const auto& h : p.pre_hooks_) {
        if (h.fn == nullptr && h.literal.empty()) {
            consteval_fail("[project] empty p.pre() command");
        }
    }
    for (const auto& h : p.post_hooks_) {
        if (h.fn == nullptr && h.literal.empty()) {
            consteval_fail("[project] empty p.post() command");
        }
    }
}

} // namespace build
