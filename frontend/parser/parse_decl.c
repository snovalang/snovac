/* parse_decl.c — dispatch for keyword-led and contextual declarations. */
#include "parse_internal.h"

/* parse_decl.c — declarations: modifiers, the by-text declaration forms
 * (`type`, `constructor`, `extension`) and the keyword-led ones. */
#include "parse_internal.h"

int decl_kind_of(SnTokKind k, SnDeclKind *out) {
    switch (k) {
    case SN_TOK_CLASS:     *out = SN_DECL_CLASS;     return 1;
    case SN_TOK_STRUCT:    *out = SN_DECL_STRUCT;    return 1;
    case SN_TOK_ENUM:      *out = SN_DECL_ENUM;      return 1;
    case SN_TOK_INTERFACE: *out = SN_DECL_INTERFACE; return 1;
    case SN_TOK_METHOD:    *out = SN_DECL_METHOD;    return 1;
    case SN_TOK_FUNC:      *out = SN_DECL_FUNC;      return 1;
    case SN_TOK_LET:
    case SN_TOK_VAR:       *out = SN_DECL_FIELD;     return 1;
    case SN_TOK_CONST:     *out = SN_DECL_CONST;     return 1;
    default: return 0;
    }
}

static SnDecl *parse_by_text_decl(P *p, int in_type_body, SnSpan span,
                                   SnVisibility vis) {
    if (in_type_body && at_name(p) && peek_at(p, 1)->kind == SN_TOK_COLON &&
        !at(p, SN_TOK_METHOD) && !at(p, SN_TOK_FUNC)) {
        return parse_bare_field(p, span);
    }
    if (!at(p, SN_TOK_IDENT)) {
        return NULL;
    }
    const char *w = cur(p)->text;
    if (in_type_body && strcmp(w, "constructor") == 0 &&
        peek_at(p, 1)->kind == SN_TOK_LPAREN) {
        return parse_constructor(p, span);
    }
    if (!in_type_body && strcmp(w, "extension") == 0 &&
        at_name_tok(peek_at(p, 1)->kind)) {
        return parse_extension(p, span, vis);
    }
    if (strcmp(w, "type") == 0 && at_name_tok(peek_at(p, 1)->kind)) {
        return parse_type_alias(p, span);
    }
    return NULL;
}

SnDecl *parse_decl(P *p, int in_type_body) {
    SnList decorators = {0};
    parse_decorators(p, &decorators);

    SnSpan span = cur(p)->span;
    Modifiers mods = parse_modifiers(p);

    SnDecl *by_text = parse_by_text_decl(p, in_type_body, span, mods.vis);
    if (by_text) {
        by_text->decorators = decorators;
        by_text->vis = mods.vis;
        by_text->is_static = mods.is_static;
        return by_text;
    }

    SnDeclKind k;
    if (!decl_kind_of(kind(p), &k)) {
        error_at(p, cur(p), SNOVA_EXPECTED_DECL,
                 "expected a declaration, found `%s`", sn_tok_name(kind(p)));
        return NULL;
    }

    SnDecl *d = new_decl(p, k, span);
    d->decorators = decorators;
    d->vis = mods.vis;
    d->is_static = mods.is_static;
    d->is_override = mods.is_override;
    d->is_async = mods.is_async;
    d->is_abstract = mods.is_abstract;
    d->is_pulsar = mods.is_pulsar;

    if (k == SN_DECL_FIELD) {
        d->is_mutable = at(p, SN_TOK_VAR) ? 1u : 0u;
    }
    advance_p(p); /* the declaration keyword */

    switch (k) {
    case SN_DECL_CLASS:
    case SN_DECL_STRUCT:
    case SN_DECL_INTERFACE:
        d->name = expect_name(p);
        if (at(p, SN_TOK_LT)) {
            parse_generic_params(p, &d->generics);
        }
        /* `class Foo(a: int)` primary-constructor form is read and kept only
         * as shape until P2. */
        if (at(p, SN_TOK_LPAREN)) {
            parse_params(p, &d->params);
        }
        parse_supertypes(p, d, 1);
        /* A bodyless type is an opaque native handle: `@native struct Scheduler`.
         * Requiring `@native` on it is a semantic rule owned by P2, exactly as
         * for a bodyless method — the grammar only records the absence. */
        if (at(p, SN_TOK_LBRACE)) {
            parse_type_body(p, d);
        }
        return d;

    case SN_DECL_ENUM:
        d->name = expect_name(p);
        if (at(p, SN_TOK_LT)) {
            parse_generic_params(p, &d->generics);
        }
        parse_supertypes(p, d, 0); /* `enum MathError : Error { ... }` */
        parse_enum_body(p, d);
        return d;

    case SN_DECL_METHOD:
    case SN_DECL_FUNC:
        parse_routine_rest(p, d);
        return d;

    case SN_DECL_FIELD:
    case SN_DECL_CONST:
        d->name = expect_name(p);
        if (accept(p, SN_TOK_COLON)) {
            d->type = parse_type(p);
        }
        if (accept(p, SN_TOK_ASSIGN)) {
            d->init = parse_expr(p);
        }
        /* `private let password: string { get; set; }` — a property block. Only
         * a member can carry one, and only a field: at top level a `{` after a
         * declaration is never part of it, so the check stays narrow rather
         * than swallowing whatever follows. */
        if (k == SN_DECL_FIELD && in_type_body && at(p, SN_TOK_LBRACE)) {
            parse_accessor_block(p, d);
        }
        accept(p, SN_TOK_SEMI);
        return d;

    default:
        break;
    }
    return d;
}
