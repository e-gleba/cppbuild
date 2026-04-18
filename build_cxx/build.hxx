// ─────────────────────────────────────────────────────────────────────────
//  build_cxx/build.hxx — umbrella header. Users include only this one.
//
//  Pieces:
//    primitives.hxx   fixed-capacity consteval containers + path helpers
//    config.hxx       enums + flag projections
//    target.hxx       a build target (exe / static / shared)
//    project.hxx      a set of targets + globals + defaults
//    validate.hxx     consteval validation
//    emit.hxx         command-string generation (+ depfiles)
//    plan.hxx         linearized plan (steps + compile db)
//    runtime.hxx      runtime helpers, clang/gcc introspection
//    executor.hxx     static-init executor (parallel, incremental)
//
//  USING SUBDIRECTORIES (no macros, plain C++).
//  In your top-level build.cxx:
//
//      #include "build_cxx/build.hxx"
//      #include "src/test/build.cxx"
//      // ...
//      consteval auto configure() -> build::project {
//          build::project proj("app", { .major = 1 });
//          // ...
//          build::subdir::src_test::configure(proj);
//          // ...
//      }
//
//  In the sub file itself (three boilerplate lines, all plain C++):
//
//      namespace build::subdir::src_test {
//      inline consteval void configure(build::project& proj) {
//          proj.add_static_library("greeting")
//              .add_sources({ "hello.cpp" });
//      }
//      }
//
//  Paths in add_sources / add_include_dirs resolve relative to the file
//  that wrote them via std::source_location, so subdir files use bare
//  filenames.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include "errors.hxx"
#include "primitives.hxx"
#include "config.hxx"
#include "target.hxx"
#include "project.hxx"
#include "validate.hxx"
#include "emit.hxx"
#include "plan.hxx"
#include "runtime.hxx"
#include "expand.hxx"
#include "external.hxx"
#include "executor.hxx"
#include "emit_ninja.hxx"
#include "driver.hxx"
