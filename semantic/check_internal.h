#ifndef SNOVAC_CHECK_INTERNAL_H
#define SNOVAC_CHECK_INTERNAL_H

#include "check.h"

typedef struct {
    SnSymbol **symbols;
    uint8_t *before;
    uint8_t *any_moved;
    size_t count;
} SnMoveSnapshot;
SnMoveSnapshot *sn_check_move_snapshot_new(SnChecker *checker, SnScope *scope);
void sn_check_move_snapshot_restore(const SnMoveSnapshot *snapshot);
void sn_check_move_snapshot_accumulate(SnMoveSnapshot *snapshot);
void sn_check_move_snapshot_apply(const SnMoveSnapshot *snapshot);

int is_numeric(const SnTypeRep *type);
SnSymbol *find_type_symbol_anywhere(SnChecker *checker,
                                    const SnDecl *context_decl,
                                    const char *name);
SnSymbol *find_variant_owner_symbol(SnResolver *resolver,
                                    const SnSymbol *variant_symbol);
SnTypeRep *func_like_type(SnChecker *checker, const SnDecl *decl);
SnTypeRep *resolve_declared_type(SnChecker *checker, const SnSymbol *symbol,
                                 const SnType *type);
SnTypeRep *symbol_type(SnChecker *checker, SnSymbol *symbol);
int is_mutable_symbol(const SnChecker *checker, const SnSymbol *symbol);
SnSymbol *lookup_member_with_inheritance(SnChecker *checker,
                                         const SnDecl *type_decl,
                                         const char *name);
const char *resolve_package_prefix(SnChecker *checker, SnScope *local,
                                   SnExpr *expr);
SnTypeRep *intrinsic_generic_ref(SnChecker *checker, const char *name,
                                 const SnList *type_args);
SnTypeRep *intrinsic_type_ref(SnChecker *checker, SnExpr *expr);
SnSymbol *resolve_value_symbol(SnChecker *checker, SnScope *local,
                               SnExpr *expr);
SnTypeRep *check_binary(SnChecker *checker, SnExpr *expr, SnTypeRep *left,
                        SnTypeRep *right);
SnTypeRep *check_expr_call(SnChecker *checker, SnScope *local, SnExpr *expr);
SnTypeRep *check_expr_control(SnChecker *checker, SnScope *local, SnExpr *expr);
SnTypeRep *sn_check_expr_consume(SnChecker *checker, SnScope *local, SnExpr *expr);
void sn_check_mark_expr_consumed(SnChecker *checker, SnExpr *expr, SnTypeRep *type);
int sn_check_type_is_owned(const SnTypeRep *type);
int sn_check_expr_has_borrowed_origin(const SnExpr *expr);
SnTypeRep *check_unary(SnChecker *checker, SnExpr *expr, SnTypeRep *operand);
void check_call_args(SnChecker *checker, SnExpr *call_expr,
                     const SnSymbol *callee, const SnList *params,
                     SnTypeRep **arg_types, size_t arg_count);
void check_call_against_functype(SnChecker *checker, SnExpr *call_expr,
                                 const SnTypeRep *function_type,
                                 SnTypeRep **arg_types, size_t arg_count);
SnTypeRep *named_with_type_args(SnChecker *checker, SnSymbol *type_symbol,
                                const SnExpr *call);
void check_constructor_call(SnChecker *checker, SnExpr *call_expr,
                            SnSymbol *type_symbol, SnTypeRep **arg_types,
                            size_t arg_count);

int sn_check_is_error(const SnTypeRep *type);
int sn_check_types_clash(SnChecker *checker, const SnTypeRep *value,
                         const SnTypeRep *expected);
int sn_check_argument_type_clash(SnChecker *checker, const SnTypeRep *value,
                                 const SnTypeRep *expected);
SnTypeRep *sn_check_supported_borrow_target(const SnTypeRep *type);
int sn_check_type_contains_reference(const SnTypeRep *type);
SnTypeRep *sn_check_adopt_literal_type(SnChecker *checker, SnExpr *expr,
                                       SnTypeRep *actual, SnTypeRep *expected);
SnSymbol *sn_check_find_type_symbol_for_decl(SnResolver *resolver,
                                             const SnDecl *type_decl);
void sn_check_bind_pattern_names(SnChecker *checker, SnScope *scope,
                                const SnPattern *pattern,
                                SnTypeRep *subject_type,
                                int subject_is_borrowed);
int sn_check_match_exhaustiveness(SnChecker *checker,
                                 const SnTypeRep *target_type,
                                 const SnList *arms, SnSpan span);
int sn_check_stmt_can_exit_loop(const SnStmt *stmt);
int sn_check_stmt_always_returns(const SnStmt *stmt);
int sn_check_stmt_is_empty(const SnStmt *stmt);
int sn_check_control_stmt(SnChecker *checker, SnScope *scope, SnStmt *stmt);

#endif
