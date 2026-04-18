// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/plan.hxx — linearized build plan.
//
//  Two-phase:
//
//  * Static phase (consteval). make_plan() emits full commands for every
//    target whose sources are known at compile time and that doesn't use
//    unity build. These steps land in `steps` + `db_entries`.
//
//  * Deferred phase (runtime). Targets that need runtime information —
//    glob/regex patterns to expand against the filesystem, or unity
//    builds where the generated .cpp is written at runtime — are listed
//    in `deferred` by target index. The executor invokes a runtime
//    expander for each, producing the final steps and db entries right
//    before execution.
//
//  Step record:
//    - command      : pre-baked command line
//    - output_path  : -o target
//    - label        : owning target name
//    - primary_input: source path (compile); empty for link/archive
//    - depfile_path : .d path (compile only)
//    - is_compile   : parallelizable vs serial wave behavior
//    - is_unity     : generated unity.cpp source — executor writes it
//                     before spawning the compiler
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "emit.hxx"
#include "primitives.hxx"
#include "project.hxx"
#include "validate.hxx"

namespace build {

struct compile_db_entry final
{
    ct_string<4096> command{};
    ct_path         file{};
};

struct build_step final
{
    ct_string<4096>  command{};
    ct_string<512>   output_path{};
    ct_path          primary_input{};
    ct_string<512>   depfile_path{};
    std::string_view label{};
    bool             is_compile{ false };
    bool             is_unity{ false };
};

struct build_plan final
{
    ct_vector<build_step, 256>       steps{};
    ct_vector<compile_db_entry, 256> db_entries{};
    ct_vector<std::size_t, 32>       deferred{}; // target indices
    std::string_view                 build_dir{};
};

// True when a target must be expanded at runtime rather than compile time.
[[nodiscard]] consteval auto is_deferred(const target& t) -> bool
{
    return t.unity_ || !t.glob_patterns_.is_empty();
}

[[nodiscard]] consteval auto make_plan(const project& proj) -> build_plan
{
    validate(proj);

    build_plan plan;
    plan.build_dir = proj.build_dir_;

    for (std::size_t i = 0; i < proj.targets_.size(); ++i) {
        const auto& tgt = proj.targets_[i];

        if (is_deferred(tgt)) {
            plan.deferred.push_back(i);
            continue;
        }

        // Static path: one compile step per source, one link/archive step.
        for (std::size_t j = 0; j < tgt.sources_.size(); ++j) {
            const auto src_sv = tgt.sources_[j].view();
            build_step step;
            step.command = emit_object_command(proj, tgt, src_sv);
            append_object_path(step.output_path,
                               proj.build_dir_,
                               tgt.name_,
                               src_sv);
            append_depfile_path(step.depfile_path,
                                proj.build_dir_,
                                tgt.name_,
                                src_sv);
            step.primary_input.append(src_sv);
            step.label      = tgt.name_;
            step.is_compile = true;
            plan.steps.push_back(step);
        }

        if (tgt.kind_ == target_kind::executable) {
            build_step step;
            step.command = emit_link_command(proj, tgt);
            step.output_path.append(proj.build_dir_)
                .append('/')
                .append(tgt.name_);
            step.label = tgt.name_;
            plan.steps.push_back(step);
        } else if (tgt.kind_ == target_kind::shared_library) {
            build_step step;
            step.command = emit_shared_link_command(proj, tgt);
            step.output_path.append(proj.build_dir_)
                .append("/lib")
                .append(tgt.name_)
                .append(to_output_ext(tgt.kind_));
            step.label = tgt.name_;
            plan.steps.push_back(step);
        } else if (tgt.kind_ == target_kind::static_library) {
            build_step step;
            step.command = emit_static_archive_command(proj, tgt);
            step.output_path.append(proj.build_dir_)
                .append("/lib")
                .append(tgt.name_)
                .append(to_output_ext(tgt.kind_));
            step.label = tgt.name_;
            plan.steps.push_back(step);
        }

        for (std::size_t j = 0; j < tgt.sources_.size(); ++j) {
            const auto src_sv = tgt.sources_[j].view();
            compile_db_entry e;
            e.command = emit_file_command(proj, tgt, src_sv);
            e.file.append(src_sv);
            plan.db_entries.push_back(e);
        }
    }
    return plan;
}

} // namespace build
