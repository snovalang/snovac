#include "check.h"
#include "check_internal.h"

#include <string.h>

int sn_check_match_exhaustiveness(SnChecker *c, const SnTypeRep *target_ty,
                                  const SnList *arms, SnSpan span) {
    if (!target_ty || sn_check_is_error(target_ty) || sn_type_is_any(target_ty) || !arms || arms->len == 0) {
        return 0;
    }
    for (size_t i = 0; i < arms->len; i++) {
        const SnMatchArm *arm = SN_LIST_AT(*arms, SnMatchArm, i);
        if (arm->guard) continue;
        if (!arm->pattern || arm->pattern->kind == SN_PAT_WILDCARD ||
            arm->pattern->kind == SN_PAT_BINDING) {
            return 1; /* Catch-all / wildcard covers all remaining branches */
        }
    }
    if (target_ty->tag == SN_T_NAMED && target_ty->decl && target_ty->decl->decl &&
        target_ty->decl->decl->kind == SN_DECL_ENUM) {
        const SnDecl *enum_decl = target_ty->decl->decl;
        size_t total_variants = enum_decl->members.len;
        if (total_variants == 0) return 0;

        for (size_t vi = 0; vi < total_variants; vi++) {
            const SnDecl *v = SN_LIST_AT(enum_decl->members, SnDecl, vi);
            int covered = 0;
            for (size_t ai = 0; ai < arms->len; ai++) {
                const SnMatchArm *arm = SN_LIST_AT(*arms, SnMatchArm, ai);
                if (arm->guard) continue;
                if (arm->pattern && arm->pattern->kind == SN_PAT_VARIANT &&
                    arm->pattern->name && v->name &&
                    strcmp(arm->pattern->name, v->name) == 0) {
                    covered = 1;
                    break;
                }
            }
            if (!covered) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NON_EXHAUSTIVE_MATCH, span,
                             "non-exhaustive match on `%s`: missing variant `%s`",
                             enum_decl->name, v->name);
                return 0;
            }
        }
        return 1;
    }
    return 0;
}
