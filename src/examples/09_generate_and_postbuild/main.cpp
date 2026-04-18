// Generated header lives in <build_dir>/generated/ex09/version.hpp. We
// didn't add any -I ourselves; the build system auto-adds that include
// root for every target in a project that calls generate_file(...).
#include "ex09/version.hpp"

#include <print>

int main()
{
    std::println("[09_generate_and_postbuild] {} {} by {} ({}) root={}",
                 ex09::project,
                 ex09::version,
                 ex09::author,
                 ex09::commit,
                 ex09::root);
    return 0;
}
