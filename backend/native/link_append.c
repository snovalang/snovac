/* link_append.c — compatibility entry point for standalone executable builds. */
#include "link_append.h"

#include "native_backend.h"

int sn_build_executable(const SnBCUnit *bc, const char *output_path) {
    if (!bc || !output_path) return 0;
    SnTargetInfo target = sn_target_get_active();
    return sn_native_compile(bc, &target, output_path);
}
