// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/executor.hxx — runtime consumer of the plan.
//
//  Split into methods so the driver (see driver.hxx) can dispatch CLI
//  subcommands. The old "do everything in static init" shape is gone:
//  main(argc, argv) now drives the session.
//
//  Phases (prepare → act):
//    prepare():
//      1. Probe the compiler (path, resource dir, triple, sys includes).
//      2. Write .clangd.
//      3. Materialise generate_file() outputs.
//      4. Run fetch_git + external_project.
//      5. Expand deferred targets (glob / regex / unity) into runtime
//         build_steps.
//      6. Flatten static Plan.steps + expanded steps into a single list.
//      7. Flatten compile_db entries the same way.
//      8. Write compile_commands.json.
//    run_all() / run_target() / dry_run():
//      Consumes the session without re-preparing.
//
//  Incremental:
//    - Compile step: output must exist and be newer than source + every
//      path in its depfile (-MD -MF).
//    - Link/archive: output newer than all compile outputs for the same
//      target plus all dep library files.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "errors.hxx"
#include "expand.hxx"
#include "external.hxx"
#include "plan.hxx"
#include "project.hxx"
#include "runtime.hxx"

#include <unordered_map>
#include <unordered_set>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <future>
#include <print>
#include <ranges>
#include <string>
#include <vector>

namespace build {

// ── Runtime-shape mirrors of the consteval step / db entry ──────────
// (ct_strings are fixed-capacity consteval; runtime stuff uses std::string
// for flexibility — we only carry const views of them around.)

struct runtime_step final
{
    std::string command;
    std::string output_path;
    std::string depfile_path;
    std::string primary_input;
    std::string label;
    bool        is_compile{ false };
    bool        is_unity{ false };

    static auto from_static(const build_step& s) -> runtime_step
    {
        return { .command       = std::string(s.command.c_str()),
                 .output_path   = std::string(s.output_path.c_str()),
                 .depfile_path  = std::string(s.depfile_path.c_str()),
                 .primary_input = std::string(s.primary_input.c_str()),
                 .label         = std::string(s.label),
                 .is_compile    = s.is_compile,
                 .is_unity      = s.is_unity };
    }
};

struct runtime_db_entry final
{
    std::string command;
    std::string file;
};

template <auto const& Plan, auto const& Project> struct executor final
{
    // One bundle of state a session carries around. prepare() produces
    // this; every other method consumes it.
    struct session final
    {
        compiler_probe                cp{};
        std::string                   cwd{};
        std::vector<runtime_step>     flat_steps{};
        std::vector<runtime_db_entry> flat_db{};
    };

    struct step_result final { bool ok{ false }; bool cached{ false }; };

    [[nodiscard]] static auto env_flag(const char* name) -> bool
    {
        const char* s = std::getenv(name);
        return s && s[0] != '\0' && s[0] != '0';
    }
    [[nodiscard]] static auto quiet() -> bool
    {
        static const bool v = env_flag("CPPBUILD_QUIET");
        return v;
    }
    [[nodiscard]] static auto why() -> bool
    {
        static const bool v = env_flag("CPPBUILD_WHY");
        return v;
    }
    [[nodiscard]] static auto verbose() -> bool
    {
        static const bool v = env_flag("CPPBUILD_VERBOSE");
        return v;
    }

    // Inputs of a link/archive step for staleness check.
    static auto link_inputs(const std::vector<runtime_step>& flat_steps,
                            std::size_t step_idx)
        -> std::vector<std::string>
    {
        std::vector<std::string> inputs;
        const auto& s = flat_steps[step_idx];
        for (std::size_t k = 0; k < step_idx; ++k) {
            const auto& prev = flat_steps[k];
            if (prev.is_compile && prev.label == s.label) {
                inputs.push_back(prev.output_path);
            }
        }
        if (const auto* tgt = Project.find_target(s.label)) {
            for (auto dep_name : tgt->dependencies_) {
                if (const auto* dep = Project.find_target(dep_name)) {
                    inputs.emplace_back(std::string(Project.build_dir_) +
                                        "/lib" + std::string(dep->name_) +
                                        std::string(to_output_ext(dep->kind_)));
                }
            }
        }
        return inputs;
    }

    static auto run_step(const runtime_step& step,
                         std::size_t         step_idx,
                         std::size_t         total,
                         std::size_t         log_idx,
                         const std::vector<runtime_step>& flat_steps)
        -> step_result
    {
        namespace fs = std::filesystem;
        using namespace ansi;

        std::error_code ec;
        const fs::path  outp{ step.output_path };
        if (outp.has_parent_path()) {
            fs::create_directories(outp.parent_path(), ec);
        }

        std::vector<std::string> inputs;
        if (step.is_compile) {
            inputs.push_back(step.primary_input);
            auto dep_inputs = parse_depfile(fs::path(step.depfile_path));
            inputs.insert(inputs.end(),
                          std::make_move_iterator(dep_inputs.begin()),
                          std::make_move_iterator(dep_inputs.end()));
        } else {
            inputs = link_inputs(flat_steps, step_idx);
        }
        const auto st = check_staleness(
            outp, inputs, step.command, std::string(Plan.build_dir));

        if (st) {
            std::println(stderr,
                         "{}▸{} [{}/{:d}] {}{}{}  {}(cached) {}{}",
                         cyan, reset, log_idx, total,
                         bold, step.label, reset,
                         grey, step.output_path, reset);
            return { .ok = true, .cached = true };
        }

        std::println(stderr,
                     "{}▸{} [{}/{:d}] {}{}{}{}",
                     cyan, reset, log_idx, total,
                     bold, step.label, reset,
                     step.is_unity ? "  (unity)" : "");
        if (why()) {
            std::println(stderr, "  {}why:{} {}", grey, reset, st.reason);
        }
        if (!quiet()) {
            std::println(stderr, "  {}{}{}", yellow, step.command, reset);
        }

        const int wait_status = std::system(step.command.c_str());
        if (wait_status != 0) {
            const int rc = portable::decode_exit(wait_status);
            std::println(stderr,
                         "  {}✗ FAILED{} (exit {}) — {}\n",
                         red, reset, rc, step.output_path);
            return { .ok = false, .cached = false };
        }
        if (!quiet()) {
            std::println(stderr, "  {}✓{} {}", green, reset, step.output_path);
        }
        update_stamp(outp, step.command, std::string(Plan.build_dir));
        return { .ok = true, .cached = false };
    }

    // Shared impl for target.before / target.after. `label` is the
    // printed phase name ("pre-build" / "post-build").
    static auto run_target_hooks(const ct_vector<target::post_entry, 8>& hooks,
                                 std::string_view output_path,
                                 std::string_view label) -> bool
    {
        namespace fs = std::filesystem;
        using namespace ansi;
        if (hooks.is_empty()) return true;
        const auto root       = fs::current_path().string();
        const auto build_dir  = std::string(Project.build_dir_);
        const target_ctx tctx{
            .out = output_path, .build_dir = build_dir, .root = root };
        for (const auto& pe : hooks) {
            std::string cmd;
            if (pe.fn != nullptr) cmd = pe.fn(tctx);
            else                  cmd = std::string(pe.literal);
            std::println(stderr, "  {}{}{} {}", cyan, label, reset, cmd);
            const int wait_status = std::system(cmd.c_str());
            if (wait_status != 0) {
                std::println(stderr,
                             "  {}✗ {} FAILED{} (exit {})",
                             red, label, reset,
                             portable::decode_exit(wait_status));
                return false;
            }
        }
        return true;
    }

    static auto run_pre_build(const target& tgt,
                              std::string_view output_path) -> bool
    { return run_target_hooks(tgt.pre_build_, output_path, "pre-build"); }

    static auto run_post_build(const target& tgt,
                               std::string_view output_path) -> bool
    { return run_target_hooks(tgt.post_build_, output_path, "post-build"); }

    // Project-level pre / post hooks. Run once per invocation; always
    // run (cache-hit does not skip them). Used for bracket actions
    // around the whole build.
    static auto run_project_hooks(
        const ct_vector<project_hook_spec, 8>& hooks,
        std::string_view label) -> bool
    {
        namespace fs = std::filesystem;
        using namespace ansi;
        if (hooks.is_empty()) return true;
        const auto root      = fs::current_path().string();
        const auto build_dir = std::string(Project.build_dir_);
        // Version string built once per call.
        const auto ver = std::format("{}.{}.{}",
                                     Project.version_.major,
                                     Project.version_.minor,
                                     Project.version_.patch);
        const project_ctx pctx{
            .project   = Project.name_,
            .version   = ver,
            .build_dir = build_dir,
            .root      = root,
        };
        for (const auto& h : hooks) {
            std::string cmd;
            if (h.fn != nullptr) cmd = h.fn(pctx);
            else                 cmd = std::string(h.literal);
            std::println(stderr, "{}●{} {}{}{} {}",
                         cyan, reset, bold, label, reset, cmd);
            const int wait_status = std::system(cmd.c_str());
            if (wait_status != 0) {
                std::println(stderr,
                             "{}✗ project {} FAILED{} (exit {})",
                             red, label, reset,
                             portable::decode_exit(wait_status));
                return false;
            }
        }
        return true;
    }

    // ── prepare: produce a session ready to be executed or emitted ─────
    [[nodiscard]] static auto prepare() -> session
    {
        namespace fs = std::filesystem;
        using namespace ansi;

        session ses{};
        fs::create_directories(Plan.build_dir);

        std::println(stderr,
                     "\n{}●{} {}{}{} v{:d}.{:d}.{:d}",
                     cyan, reset, bold,
                     std::string_view(Project.name_), reset,
                     Project.version_.major,
                     Project.version_.minor,
                     Project.version_.patch);

        constexpr auto compiler_sv = to_flag(Project.compiler_);
        const std::string compiler_name(compiler_sv);
        ses.cp  = probe(compiler_name);
        ses.cwd = fs::current_path().string();

        std::println(stderr, "  {}compiler{} : {}", bold, reset, ses.cp.absolute_path);
        if (!ses.cp.version.empty()) {
            std::println(stderr, "  {}version {} : {}", bold, reset, ses.cp.version);
        }
        std::println(stderr,
                     "  {}triple  {} : {}", bold, reset,
                     ses.cp.target_triple.empty() ? "(unknown)"
                                                  : ses.cp.target_triple.c_str());
        std::println(stderr,
                     "  {}resource{} : {}", bold, reset,
                     ses.cp.resource_dir.empty() ? "(n/a)"
                                                 : ses.cp.resource_dir.c_str());
        std::println(stderr,
                     "  {}sys-incs{} : {} paths", bold, reset,
                     ses.cp.system_includes.size());

        write_clangd_config(Plan.build_dir);
        std::println(stderr,
                     "\n{}▸{} generated {}.clangd{}", cyan, reset, bold, reset);

        // Generated files. Literal content passes through verbatim.
        // Callables are invoked with gen_ctx; they typically use
        // std::format to embed the project name / version / root.
        const auto gen_dir = fs::path(std::string(Plan.build_dir)) / "generated";
        if (!Project.generated_.is_empty()) {
            fs::create_directories(gen_dir);
            char ver_buf[32]{};
            std::snprintf(ver_buf, sizeof ver_buf, "%d.%d.%d",
                          Project.version_.major,
                          Project.version_.minor,
                          Project.version_.patch);
            const gen_ctx gctx{
                .project = Project.name_,
                .version = ver_buf,
                .root    = ses.cwd,
            };
            for (const auto& g : Project.generated_) {
                std::string rendered = g.is_dynamic()
                    ? g.fn(gctx)
                    : std::string(g.literal_content.view());
                const auto abs_path = gen_dir / std::string(g.rel_path.view());
                const bool wrote    = write_if_different(abs_path, rendered);
                std::println(stderr,
                             "{}▸{} generate {}{}{}  {}{}{}",
                             cyan, reset, bold, abs_path.string(), reset,
                             grey, wrote ? "(written)" : "(up-to-date)", reset);
            }
        }

        // fetch_git + external_project.
        std::unordered_map<std::string, bool> source_is_new;
        for (const auto& f : Project.fetches_) {
            source_is_new[std::string(f.name)] =
                run_fetch(f, Plan.build_dir);
        }
        for (const auto& e : Project.externals_) {
            bool src_new = false;
            if (!e.source_name.empty()) {
                auto it = source_is_new.find(std::string(e.source_name));
                if (it != source_is_new.end()) src_new = it->second;
            }
            run_external(e, Plan.build_dir, src_new);
        }

        // Expand deferred targets (glob / regex / unity).
        std::vector<build_step>       extra_steps;
        std::vector<compile_db_entry> extra_db;
        for (std::size_t idx : Plan.deferred) {
            const auto& tgt = Project.targets_[idx];
            std::vector<std::string> final_sources;
            expand_deferred_target(
                Project, tgt, final_sources, extra_steps, extra_db);
            if (final_sources.empty()) {
                die("target has no sources after expansion",
                    std::string(tgt.name_));
            }
            if (tgt.unity_) {
                (void) write_unity_file(
                    Project.build_dir_, tgt.name_, final_sources);
            }
            std::println(stderr,
                         "{}▸{} expanded {}{}{} → {} source(s){}",
                         cyan, reset, bold,
                         std::string_view(tgt.name_), reset,
                         final_sources.size(),
                         tgt.unity_ ? "  (unity)" : "");
        }

        // Flatten steps + db.
        ses.flat_steps.reserve(Plan.steps.size() + extra_steps.size());
        for (const auto& s : Plan.steps)  ses.flat_steps.push_back(runtime_step::from_static(s));
        for (const auto& s : extra_steps) ses.flat_steps.push_back(runtime_step::from_static(s));

        ses.flat_db.reserve(Plan.db_entries.size() + extra_db.size());
        auto push_db = [&](std::string_view cmd_sv, std::string_view file_sv) {
            ses.flat_db.push_back({ std::string(cmd_sv), std::string(file_sv) });
        };
        for (const auto& e : Plan.db_entries) push_db(e.command.view(), e.file.view());
        for (const auto& e : extra_db)        push_db(e.command.view(), e.file.view());

        // Add every `build.cxx` (root + subdirs) to the compile DB so
        // clangd gives precise flags when editing the build system
        // itself. Command shape matches the bootstrap compile:
        //   clang++ -std=c++26 -I<root> -c <file>
        // No -o / -MD so clangd sees a stable signature.
        {
            std::string header;
            header.append("clang++ -std=c++26 -I").append(ses.cwd);
            header.append(" -c ");
            auto add_build_cxx = [&](const fs::path& abs) {
                std::string cmd = header;
                cmd.append(abs.string());
                ses.flat_db.push_back({ std::move(cmd), abs.string() });
            };
            // Root build.cxx lives at cwd.
            const auto root_bld = fs::path(ses.cwd) / "build.cxx";
            if (fs::exists(root_bld)) add_build_cxx(root_bld);
            // Find every subdir build.cxx. Skip build/ (object files) and
            // hidden dirs (.git, .cache). fs::recursive_directory_iterator
            // follows regular dirs only by default.
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(ses.cwd, ec);
                 !ec && it != fs::recursive_directory_iterator(); ++it) {
                auto& entry = *it;
                const auto& p = entry.path();
                const auto  fname = p.filename().string();
                if (entry.is_directory() && (fname.starts_with('.') ||
                                             fname == "build" ||
                                             fname == "_deps")) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (entry.is_regular_file() && fname == "build.cxx" &&
                    fs::absolute(p) != root_bld) {
                    add_build_cxx(fs::absolute(p));
                }
            }
        }

        write_compile_db(ses);
        write_ids_header(ses);
        return ses;
    }

    // build/targets.hpp — C++ header enumerating every declared target
    // and runnable as a constexpr std::string_view. Optional opt-in for
    // users who want clangd autocomplete and compile-time-checked
    // cross-references instead of raw string literals:
    //
    //   #include "build/targets.hpp"
    //   p.exe("app").dep(targets::mylib);   // typo -> compile error
    //   ./build.cxx --run runnables::test   // typo -> compile error
    //
    // Regenerated every prepare(); content-aware so mtime stays stable
    // when the target list hasn't changed (keeps IDE caches warm).
    static void write_ids_header(const session& /*ses*/)
    {
        namespace fs = std::filesystem;
        using namespace ansi;

        std::string body;
        body.reserve(2048);
        body += "// Auto-generated by build.cxx — do not hand-edit.\n";
        body += "// Regenerated on every `./build.cxx` run.\n";
        body += "//\n";
        body += "// Use these instead of raw string literals for\n";
        body += "// compile-time-checked references:\n";
        body += "//\n";
        body += "//   #include \"build/targets.hpp\"\n";
        body += "//   p.exe(\"myapp\").dep(targets::mylib);\n";
        body += "#pragma once\n";
        body += "#include <string_view>\n\n";
        body += "namespace targets {\n";
        for (const auto& t : Project.targets_) {
            body += std::format(
                "    inline constexpr std::string_view {} = \"{}\";\n",
                std::string_view(t.name_), std::string_view(t.name_));
        }
        body += "} // namespace targets\n\n";
        body += "namespace runnables {\n";
        for (const auto& r : Project.runnables_) {
            body += std::format(
                "    inline constexpr std::string_view {} = \"{}\";\n",
                std::string_view(r.name), std::string_view(r.name));
        }
        body += "} // namespace runnables\n";

        const auto path =
            fs::path(std::string(Plan.build_dir)) / "targets.hpp";
        const bool wrote = write_if_different(path, body);
        std::println(stderr,
                     "{}▸{} {}targets.hpp{}  {}{} ({} target(s), {} runnable(s)){}",
                     cyan, reset, bold, reset, grey,
                     wrote ? "(written)" : "(up-to-date)",
                     Project.targets_.size(), Project.runnables_.size(), reset);
    }

    // compile_commands.json — enriched with the probed resource-dir and
    // system includes so clangd sees the same picture the compiler does.
    static void write_compile_db(const session& ses)
    {
        namespace fs = std::filesystem;
        using namespace ansi;
        const auto db_path =
            fs::path{ Plan.build_dir } / "compile_commands.json";
        std::FILE* db = std::fopen(db_path.c_str(), "w");
        if (!db) die("cannot write compile_commands.json", db_path.string());

        std::size_t db_count = 0;
        std::println(db, "[");
        for (const auto& entry : ses.flat_db) {
            auto args = tokenize(entry.command);
            if (!args.empty()) args[0] = ses.cp.absolute_path;
            if (!ses.cp.resource_dir.empty()) {
                args.insert(args.begin() + 1, "-resource-dir");
                args.insert(args.begin() + 2, ses.cp.resource_dir);
            }
            for (const auto& inc : ses.cp.system_includes) {
                args.push_back("-isystem");
                args.push_back(inc);
            }
            auto file_abs = fs::absolute(fs::path(entry.file)).string();
            if (db_count > 0) std::println(db, ",");
            std::println(db, "  {{");
            std::println(db, R"(    "directory": "{}",)", json_escape(ses.cwd));
            std::println(db, R"(    "file": "{}",)", json_escape(file_abs));
            std::println(db, "    \"arguments\": [");
            for (std::size_t a = 0; a < args.size(); ++a) {
                std::println(db,
                             "      \"{}\"{}",
                             json_escape(args[a]),
                             (a + 1 < args.size()) ? "," : "");
            }
            std::print(db, "    ]\n  }}");
            ++db_count;
        }
        std::println(db, "\n]");
        std::fclose(db);

        std::println(stderr,
                     "\n{}▸{} {}compile_commands.json{} → {} "
                     "({:d} entries, {} -isystem paths)\n",
                     cyan, reset, bold, reset,
                     db_path.c_str(), db_count,
                     ses.cp.system_includes.size());

        // Root-level symlink (or copy if symlinks aren't permitted).
        const auto      src = fs::absolute(db_path);
        const auto      dst = fs::path{ "compile_commands.json" };
        std::error_code ec;
        fs::remove(dst, ec);
        fs::create_symlink(src, dst, ec);
        if (ec) {
            // symlink failed (read-only FS / Windows w/o devmode) → copy.
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        }
    }

    // True if any step in flat_steps[i..j) would be non-cached right
    // now. Used to decide whether to fire a target's `.before(...)`
    // hook. Duplicates the staleness check run_step does — that's OK,
    // it's just stat() calls, negligible cost.
    static auto wave_has_stale_step(const session&   ses,
                                    std::size_t      i,
                                    std::size_t      j) -> bool
    {
        namespace fs = std::filesystem;
        for (std::size_t k = i; k < j; ++k) {
            const auto& step = ses.flat_steps[k];
            std::vector<std::string> inputs;
            if (step.is_compile) {
                inputs.push_back(step.primary_input);
                auto dep_inputs = parse_depfile(fs::path(step.depfile_path));
                inputs.insert(inputs.end(),
                              std::make_move_iterator(dep_inputs.begin()),
                              std::make_move_iterator(dep_inputs.end()));
            } else {
                inputs = link_inputs(ses.flat_steps, k);
            }
            const auto st = check_staleness(
                fs::path{ step.output_path }, inputs, step.command,
                std::string(Plan.build_dir));
            if (!st) return true;
        }
        return false;
    }

    // Execute one linear wave-grouped run. `only_target` restricts to a
    // target and its transitive dependencies (empty string = all).
    // `jobs` caps parallelism per compile wave (0 = unbounded).
    static auto run_filtered(const session&   ses,
                             std::string_view only_target,
                             std::size_t      jobs) -> int
    {
        namespace fs = std::filesystem;
        using namespace ansi;

        const auto wanted = expand_deps(only_target);

        const auto         t0    = std::chrono::steady_clock::now();
        const std::size_t  total = ses.flat_steps.size();
        std::size_t        i     = 0;
        std::size_t        ran = 0, cached = 0, skipped = 0;
        std::atomic<std::size_t> log_idx{ 0 };
        // Tracks which targets have already had their .before() fired
        // this invocation. Fire at most once per target per run.
        std::unordered_set<std::string_view> pre_fired;

        while (i < total) {
            const auto  wave_label      = ses.flat_steps[i].label;
            const bool  wave_is_compile = ses.flat_steps[i].is_compile;

            std::size_t j = i;
            while (j < total &&
                   ses.flat_steps[j].label == wave_label &&
                   ses.flat_steps[j].is_compile == wave_is_compile) {
                ++j;
            }

            // Skip whole wave if its target isn't in the wanted set.
            if (!wanted.empty() && !wanted.contains(std::string(wave_label))) {
                skipped += (j - i);
                i = j;
                continue;
            }

            // Fire target.before(...) once per target — on the first
            // wave where any of its steps would be non-cached. If the
            // target is fully cached across all waves, .before() never
            // fires. Symmetric with .after(), which fires only after a
            // non-cached link step.
            if (const auto* t = Project.find_target(wave_label);
                t != nullptr
                && !t->pre_build_.is_empty()
                && !pre_fired.contains(wave_label)
                && wave_has_stale_step(ses, i, j))
            {
                pre_fired.insert(wave_label);
                const auto& final_step =
                    ses.flat_steps[ses.flat_steps.size() - 1];
                // Best-effort output path for target_ctx.out: use the
                // last non-compile step of this target in the plan. At
                // pre-build time the file may not exist yet — that's OK,
                // it's a path string, not a file handle.
                std::string_view out_path = final_step.output_path;
                for (std::size_t k = 0; k < total; ++k) {
                    const auto& s = ses.flat_steps[k];
                    if (s.label == wave_label && !s.is_compile) {
                        out_path = s.output_path;
                        break;
                    }
                }
                if (!run_pre_build(*t, out_path)) return 1;
            }

            const std::size_t wave_size = j - i;
            const bool        parallel  = wave_is_compile && wave_size > 1;
            const std::size_t cap       = jobs == 0 ? wave_size
                                                    : std::min(jobs, wave_size);

            if (parallel) {
                // Bounded parallelism: run `cap` futures at a time.
                bool ok = true;
                for (std::size_t k = i; k < j; k += cap) {
                    const std::size_t chunk_end = std::min(k + cap, j);
                    std::vector<std::future<step_result>> futs;
                    futs.reserve(chunk_end - k);
                    for (std::size_t m = k; m < chunk_end; ++m) {
                        auto my_log = ++log_idx;
                        futs.push_back(std::async(
                            std::launch::async,
                            [m, my_log, total, &ses] {
                                return run_step(
                                    ses.flat_steps[m], m, total, my_log,
                                    ses.flat_steps);
                            }));
                    }
                    for (auto& f : futs) {
                        auto r = f.get();
                        ok     = r.ok && ok;
                        (r.cached ? cached : ran) += 1;
                    }
                    if (!ok) break;
                }
                if (!ok) return 1;
            } else {
                for (std::size_t k = i; k < j; ++k) {
                    auto my_log = ++log_idx;
                    auto r = run_step(
                        ses.flat_steps[k], k, total, my_log, ses.flat_steps);
                    (r.cached ? cached : ran) += 1;
                    if (!r.ok) return 1;
                    if (!wave_is_compile && !r.cached) {
                        if (const auto* t =
                                Project.find_target(ses.flat_steps[k].label)) {
                            if (!run_post_build(*t,
                                                ses.flat_steps[k].output_path)) {
                                return 1;
                            }
                        }
                    }
                }
            }
            i = j;
        }
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();

        std::println(stderr,
                     "\n{}● build complete{} — {:d} step(s): "
                     "{}{} ran{}, {}{} cached{}{}  "
                     "{}({}.{:03d}s){}\n",
                     green, reset,
                     total - skipped,
                     bold, ran, reset,
                     grey, cached, reset,
                     skipped ? std::format(", {} skipped", skipped) : "",
                     grey,
                     elapsed_ms / 1000,
                     static_cast<int>(elapsed_ms % 1000),
                     reset);
        return 0;
    }

    // Compute target + its transitive deps. Empty set = run everything.
    static auto expand_deps(std::string_view root)
        -> std::unordered_set<std::string>
    {
        using namespace ansi;
        std::unordered_set<std::string> out;
        if (root.empty()) return out;

        std::vector<std::string_view> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            auto n = stack.back(); stack.pop_back();
            auto [it, inserted] = out.insert(std::string(n));
            if (!inserted) continue;
            if (const auto* t = Project.find_target(n)) {
                for (auto d : t->dependencies_) stack.push_back(d);
            }
        }
        if (verbose()) {
            std::println(stderr,
                         "  {}dep-walk:{} {} -> {{{}}}",
                         grey, reset, root, join(out, ", "));
        }
        return out;
    }

    // ── Print-only modes ───────────────────────────────────────────────
    static void print_targets()
    {
        using namespace ansi;
        std::println(stderr, "\n{}●{} {}{}{} targets:",
                     cyan, reset, bold,
                     std::string_view(Project.name_), reset);
        for (const auto& t : Project.targets_) {
            const char* kind = "";
            switch (t.kind_) {
                case target_kind::executable:     kind = "exe";    break;
                case target_kind::static_library: kind = "lib";    break;
                case target_kind::shared_library: kind = "shared"; break;
            }
            std::println(stderr, "  {}{:<7}{} {}", grey, kind, reset,
                         std::string_view(t.name_));
        }
        if (!Project.runnables_.is_empty()) {
            std::println(stderr, "\n{}●{} runnables (invoke with --run <name>):",
                         cyan, reset);
            for (const auto& r : Project.runnables_) {
                const auto suffix = r.deps.is_empty()
                    ? std::string{}
                    : std::format("  {}(deps: {}){}",
                                  grey, join(r.deps, ", "), reset);
                std::println(stderr, "  {}{:<7}{} {}{}",
                             grey, "run", reset,
                             std::string_view(r.name), suffix);
            }
        }
        std::println(stderr, "");
    }

    static void print_plan(const session& ses)
    {
        using namespace ansi;
        std::println(stderr, "\n{}●{} plan — {} step(s):",
                     cyan, reset, ses.flat_steps.size());
        for (std::size_t i = 0; i < ses.flat_steps.size(); ++i) {
            const auto& s = ses.flat_steps[i];
            std::println(stderr,
                         "  [{:>3}] {}{}{} {}{}{} → {}",
                         i,
                         bold, s.label, reset,
                         grey, s.is_compile ? "compile" : "link", reset,
                         s.output_path);
        }
        std::println(stderr, "");
    }

    // Run a named runnable: build its transitive target deps (if any),
    // then execute each command serially. Commands are either literal
    // strings or captureless callables taking build::ctx; the callable
    // builds the command at runtime, typically via std::format. Exit
    // non-zero on the first failing command; cached target builds count
    // as success.
    static auto run_runnable(const session&       ses,
                             const runnable_spec& r,
                             std::size_t          jobs) -> int
    {
        using namespace ansi;

        if (!r.deps.is_empty()) {
            std::println(stderr,
                         "\n{}●{} runnable {}{}{}  building {} dep(s)",
                         cyan, reset, bold, std::string_view(r.name), reset,
                         r.deps.size());
            for (const auto& d : r.deps) {
                if (!Project.find_target(d)) {
                    std::println(stderr,
                                 "{}✗{} runnable '{}': unknown dep target '{}'",
                                 red, reset, r.name, d);
                    return 2;
                }
                if (int rc = run_filtered(ses, d, jobs); rc != 0) return rc;
            }
        }

        std::println(stderr,
                     "\n{}●{} runnable {}{}{}  running {} command(s)",
                     cyan, reset, bold, std::string_view(r.name), reset,
                     r.cmds.size());
        const auto root      = std::filesystem::current_path().string();
        const auto build_dir = std::string(Project.build_dir_);
        const ctx  rctx{ .build_dir = build_dir, .root = root };
        if (verbose()) {
            std::println(stderr,
                         "  {}ctx.build_dir{} = {}\n  {}ctx.root     {} = {}",
                         grey, reset, build_dir, grey, reset, root);
        }
        std::size_t idx = 0;
        for (const auto& c : r.cmds) {
            ++idx;
            const std::string cmd = c.is_dynamic()
                ? c.fn(rctx)
                : std::string(c.literal);
            std::println(stderr, "{}▸{} [{}/{}] {}{}{}",
                         cyan, reset, idx, r.cmds.size(), yellow, cmd, reset);
            const int wait_status = std::system(cmd.c_str());
            if (wait_status != 0) {
                const int rc = portable::decode_exit(wait_status);
                std::println(stderr, "{}✗{} runnable '{}' failed (exit {})",
                             red, reset, r.name, rc);
                return rc;
            }
        }
        std::println(stderr, "{}● runnable {} done{}\n",
                     green, std::string_view(r.name), reset);
        return 0;
    }

    static void clean()
    {
        namespace fs = std::filesystem;
        using namespace ansi;
        std::error_code ec;
        const auto dir = fs::path(std::string(Plan.build_dir));
        fs::remove_all(dir, ec);
        fs::remove("compile_commands.json", ec);
        if (ec) {
            std::println(stderr, "{}✗{} clean failed: {}", red, reset, ec.message());
        } else {
            std::println(stderr, "{}▸{} cleaned {}", cyan, reset, dir.string());
        }
    }

    // Like `clean()`, but keeps expensive caches:
    //   - build/_deps/   (git checkouts + external builds)
    //   - build/.bootstrap, build/.bootstrap.d   (driver exe)
    // Everything else (obj/, artifacts, .cache/ stamps, .clangd,
    // compile_commands.json, generated/) goes. Next run only recompiles
    // our own sources — no SDL3 re-download.
    static void clean_artifacts()
    {
        namespace fs = std::filesystem;
        using namespace ansi;
        const auto dir = fs::path(std::string(Plan.build_dir));
        std::error_code ec;
        if (!fs::exists(dir, ec)) {
            std::println(stderr, "{}▸{} nothing to clean ({} absent)",
                         cyan, reset, dir.string());
            fs::remove("compile_commands.json", ec);
            return;
        }
        const std::unordered_set<std::string> keep{
            "_deps", ".bootstrap", ".bootstrap.d"
        };
        std::size_t removed = 0;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (keep.contains(entry.path().filename().string())) continue;
            fs::remove_all(entry.path(), ec);
            ++removed;
        }
        fs::remove("compile_commands.json", ec);
        std::println(stderr,
                     "{}▸{} cleaned artifacts in {} "
                     "({} entries removed, kept _deps/ and .bootstrap)",
                     cyan, reset, dir.string(), removed);
    }
};

} // namespace build
