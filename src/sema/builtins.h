/* Compiler representations for primitive types and generic aliases.
 * Method signatures and behavior are declared by the Snovalang builtin
 * library; this module contains no method/member catalog. */
#ifndef SNOVAC_BUILTINS_H
#define SNOVAC_BUILTINS_H

#include "intern.h"
#include "types.h"

/* Generic representations used by declared builtin types.
 *
 * `Array<T>` is represented by SN_T_ARRAY because that is the runtime's
 * container value. `Partial<T>` follows its source alias and resolves to T.
 *
 * Returns NULL when `iname` is not an intrinsic generic. `args` are the
 * caller's already-resolved generic arguments and are not retained. */
SnTypeRep *sn_builtin_generic(SnTypeTable *t, SnInternTable *it, const char *iname,
                              SnTypeRep **args, uint32_t nargs);

/* True for exactly the names sn_builtin_generic answers to. Callers use it to
 * decide whether to resolve a generic argument list at all, so that a
 * non-intrinsic name keeps reporting one diagnostic for itself instead of also
 * reporting its unresolved arguments. */
int sn_builtin_is_generic_name(SnInternTable *it, const char *iname);

/* Element type produced by `recv[i]`, or NULL when `recv` is not subscriptable.
 *
 * The set of subscriptable receivers is encoded by the language's intrinsic
 * value representations. Where the receiver's generic arguments say what
 * comes out, they are used
 * (`Map<string, int>["a"]` is an `int`); otherwise the result is `any` rather
 * than a guess. */
SnTypeRep *sn_builtin_index_result(SnTypeTable *t, SnInternTable *it,
                                   const SnTypeRep *recv);

/* Intrinsic primitive spellings (`int`, `string`, `int32`, …). These are
 * compiler types, not symbols declared in a `.snl` file. Tools (the resolver,
 * the language server) must use this table so a new primitive cannot be
 * accepted by `snl check` and rejected or unhighlighted by the editor. */
size_t sn_builtin_primitive_count(void);
const char *sn_builtin_primitive_name(size_t index);
const char *sn_builtin_primitive_detail(size_t index);
/* `name` is a raw C string. Returns the intrinsic type, or NULL. Does not
 * accept the legacy capitalized spellings (`Int`, `String`). */
SnTypeRep *sn_builtin_primitive_type(SnTypeTable *t, const char *name);
int sn_builtin_is_primitive_name(const char *name);

#endif /* SNOVAC_BUILTINS_H */
