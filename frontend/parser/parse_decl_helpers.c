/* parse_decl_helpers.c — declaration modifiers and reusable declaration forms. */
#include "parse_internal.h"

/* Is `w` a soft modifier — a word that only acts as one when a declaration
 * keyword follows, so it stays an ordinary identifier everywhere else?
 * `abstract` is excluded: it is recorded (see parse_modifiers), not just
 * consumed-and-discarded like the rest of this list. */
static int is_soft_modifier(const char *w) {
    return strcmp(w, "data") == 0 || strcmp(w, "unsafe") == 0 ||
           strcmp(w, "sealed") == 0 ||
           strcmp(w, "final") == 0 || strcmp(w, "open") == 0 ||
           strcmp(w, "internal") == 0;
}

static int starts_decl_kw(SnTokKind k) {
    return k == SN_TOK_CLASS || k == SN_TOK_STRUCT || k == SN_TOK_ENUM ||
           k == SN_TOK_INTERFACE || k == SN_TOK_METHOD || k == SN_TOK_FUNC ||
           k == SN_TOK_LET || k == SN_TOK_VAR;
}

Modifiers parse_modifiers(P *p) {
    Modifiers m = {SN_VIS_DEFAULT, 0, 0, 0, 0, 0};
    for (;;) {
        if (at(p, SN_TOK_PUBLIC))    { m.vis = SN_VIS_PUBLIC;    advance_p(p); continue; }
        if (at(p, SN_TOK_PRIVATE))   { m.vis = SN_VIS_PRIVATE;   advance_p(p); continue; }
        if (at(p, SN_TOK_PROTECTED)) { m.vis = SN_VIS_PROTECTED; advance_p(p); continue; }
        if (at(p, SN_TOK_STATIC))    { m.is_static = 1;   advance_p(p); continue; }
        if (at(p, SN_TOK_OVERRIDE))  { m.is_override = 1; advance_p(p); continue; }
        /* `abstract` is a soft modifier like the others (an ordinary
         * identifier everywhere else — starts_decl_kw() gates it below the
         * same way), but check.c needs to know a class was marked abstract
         * to reject `Base()` where `class Base` never gets a body, so it is
         * recorded instead of just skipped. */
        if (at(p, SN_TOK_IDENT) && strcmp(cur(p)->text, "abstract") == 0 &&
            starts_decl_kw(peek_at(p, 1)->kind)) {
            m.is_abstract = 1;
            advance_p(p);
            continue;
        }
        /* `async` is a modifier only when a declaration keyword follows;
         * otherwise it is an ordinary identifier. */
        if (at(p, SN_TOK_ASYNC) && (peek_at(p, 1)->kind == SN_TOK_METHOD ||
                                    peek_at(p, 1)->kind == SN_TOK_FUNC ||
                                    peek_at(p, 1)->kind == SN_TOK_PULSAR)) {
            m.is_async = 1;
            advance_p(p);
            continue;
        }
        /* `pulsar func worker(): unit { }` — pulsar is a declaration modifier
         * here, and a statement keyword elsewhere (SN_STMT_PULSAR in
         * parse_stmt.c, `pulsar work()`). Recorded so check.c can enforce
         * SNOVA122 (return-type shape) and SNOVA124 (no direct call). */
        if (at(p, SN_TOK_PULSAR) && (peek_at(p, 1)->kind == SN_TOK_FUNC ||
                                     peek_at(p, 1)->kind == SN_TOK_METHOD ||
                                     peek_at(p, 1)->kind == SN_TOK_ASYNC)) {
            m.is_pulsar = 1;
            advance_p(p);
            continue;
        }
        if (at(p, SN_TOK_IDENT) && is_soft_modifier(cur(p)->text) &&
            starts_decl_kw(peek_at(p, 1)->kind)) {
            advance_p(p);
            continue;
        }
        break;
    }
    return m;
}

/* Supertypes written after `:`, `extends` or `implements`. The latter two are
 * not lexer keywords — recognised by text, so they stay usable as identifiers.
 * The parser does not tell a base class from an interface: that needs symbol
 * resolution, which is P2's job. */
void parse_supertypes(P *p, SnDecl *d, int allow_keywords) {
    for (;;) {
        int is_kw = allow_keywords && at(p, SN_TOK_IDENT) &&
                    (strcmp(cur(p)->text, "extends") == 0 ||
                     strcmp(cur(p)->text, "implements") == 0) &&
                    at_name_tok(peek_at(p, 1)->kind);
        if (!is_kw && !accept(p, SN_TOK_COLON)) {
            break;
        }
        if (is_kw) {
            advance_p(p);
        }
        for (;;) {
            SnType *sup = parse_type(p);
            if (sup) {
                sn_list_push(p->arena, &d->supertypes, sup);
            }
            if (!accept(p, SN_TOK_COMMA)) {
                break;
            }
        }
        if (!allow_keywords) {
            break;
        }
    }
}

/* A `data class` body declares fields as bare `name: Type` with no `let`/`var`.
 * Recognised only inside a type body, so a stray identifier at top level still
 * reports a missing declaration. */
SnDecl *parse_bare_field(P *p, SnSpan span) {
    SnDecl *f = new_decl(p, SN_DECL_FIELD, span);
    f->name = expect_name(p);
    expect(p, SN_TOK_COLON);
    f->type = parse_type(p);
    if (accept(p, SN_TOK_ASSIGN)) {
        f->init = parse_expr(p);
    }
    if (at(p, SN_TOK_LBRACE)) {
        parse_accessor_block(p, f);
    }
    accept(p, SN_TOK_COMMA);
    accept(p, SN_TOK_SEMI);
    return f;
}

/* `constructor(description: string) { ... }` — a member whose name is fixed by
 * the by-text keyword `constructor`. Parsed as a method of that name; P2 owns
 * its special dispatch. */
SnDecl *parse_constructor(P *p, SnSpan span) {
    SnDecl *c = new_decl(p, SN_DECL_METHOD, span);
    c->name = advance_p(p)->text;
    parse_params(p, &c->params);
    if (accept(p, SN_TOK_COLON)) {
        c->ret = parse_type(p);
    }
    if (at(p, SN_TOK_LBRACE)) {
        c->body = parse_block(p);
    }
    return c;
}

/* Signature and body shared by every routine-shaped declaration once its name
 * is already known: `(params)[: T | -> T] ( { block } | = expr )`. */
void parse_routine_signature_and_body(P *p, SnDecl *d) {
    if (at(p, SN_TOK_LT)) {
        parse_generic_params(p, &d->generics);
    }
    parse_params(p, &d->params);
    /* Return type is spelled `: T` or `-> T`; both occur in the corpus. */
    if (accept(p, SN_TOK_COLON) || accept(p, SN_TOK_ARROW) ||
        accept(p, SN_TOK_TILDE_ARROW)) {
        d->ret = parse_type(p);
    }
    if (at(p, SN_TOK_LBRACE)) {
        d->body = parse_block(p);
        return;
    }
    if (accept(p, SN_TOK_ASSIGN)) {
        /* Expression body: `func main(): int = 0`. The right-hand side may
         * itself be a block (`method foo(): int = { ... }`). */
        if (at(p, SN_TOK_LBRACE)) {
            d->body = parse_block(p);
            return;
        }
        /* Wrapped in an implicit return so later phases see one shape. */
        SnStmt *ret = new_stmt(p, SN_STMT_RETURN, cur(p)->span);
        ret->expr = parse_expr(p);
        SnStmt *blk = new_stmt(p, SN_STMT_BLOCK, ret->span);
        sn_list_push(p->arena, &blk->stmts, ret);
        d->body = blk;
    }
    /* Still no body means the declaration is backed by @native. That is a
     * semantic rule checked in P2, not a syntax error here. */
}

/* `extension Counter { func isZero(): bool = value == 0 }` — adds members to
 * an existing type. Top level only, recognised by text.
 *
 * `extension Counter.isZero(): bool = value == 0` is Kotlin/C#-style sugar
 * for the same thing with exactly one member: `member_vis` (the modifiers
 * written before `extension`) belongs to that member, not to the wrapper
 * SN_DECL_EXTENSION node, so `public extension Counter.isZero()` makes
 * `isZero` public the same way `public func isZero()` would inside the
 * block form. */
SnDecl *parse_extension(P *p, SnSpan span, SnVisibility member_vis) {
    advance_p(p);
    SnDecl *x = new_decl(p, SN_DECL_EXTENSION, span);
    x->name = expect_name(p);
    if (at(p, SN_TOK_LT)) {
        parse_generic_params(p, &x->generics);
    }
    if (accept(p, SN_TOK_DOT)) {
        SnDecl *m = new_decl(p, SN_DECL_FUNC, span);
        m->vis = member_vis;
        m->name = expect_name(p);
        parse_routine_signature_and_body(p, m);
        sn_list_push(p->arena, &x->members, m);
        return x;
    }
    parse_type_body(p, x);
    return x;
}

/* `public type RowDecoder<T> = func(SqlRow) -> Result<T, E>`. `type` is not a
 * lexer keyword — it is a common identifier — so it is recognised by text plus
 * the shape that follows. */
SnDecl *parse_type_alias(P *p, SnSpan span) {
    advance_p(p);
    SnDecl *ta = new_decl(p, SN_DECL_TYPEALIAS, span);
    ta->name = expect_name(p);
    if (at(p, SN_TOK_LT)) {
        parse_generic_params(p, &ta->generics);
    }
    expect(p, SN_TOK_ASSIGN);
    ta->type = parse_type(p);
    accept(p, SN_TOK_SEMI);
    return ta;
}

void parse_routine_rest(P *p, SnDecl *d) {
    d->name = expect_name(p);
    parse_routine_signature_and_body(p, d);
}

