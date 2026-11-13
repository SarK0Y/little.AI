/* Rust-style Result<T, Error> for C (GCC/Clang: uses statement expressions).
 *
 *   typedef RESULT(Cnf) ResultCnf;
 *   ResultCnf f(void) { return OK(ResultCnf, cnf); }
 *   ResultCnf g(void) { Cnf c = TRY(ResultCnf, f()); ... }   // like `f()?`
 */
#ifndef C3SAT_RESULT_H
#define C3SAT_RESULT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef enum { ERR_IO = 1, ERR_PARSE, ERR_NOMEM, ERR_USAGE, ERR_VERIFY } ErrKind;

typedef struct {
    ErrKind kind;
    char msg[192];
} Error;

static inline const char *err_kind_name(ErrKind k) {
    switch (k) {
        case ERR_IO: return "io";
        case ERR_PARSE: return "parse";
        case ERR_NOMEM: return "nomem";
        case ERR_USAGE: return "usage";
        case ERR_VERIFY: return "verify";
    }
    return "unknown";
}

static inline Error error_make(ErrKind kind, const char *fmt, ...) {
    Error e;
    e.kind = kind;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof e.msg, fmt, ap);
    va_end(ap);
    return e;
}

/* Like anyhow's .context(): "ctx: original message" (truncated if needed). */
static inline Error error_context(Error e, const char *ctx) {
    char tmp[sizeof e.msg];
    size_t cl = strlen(ctx), ml = strlen(e.msg), n = 0;
    for (size_t i = 0; i < cl && n < sizeof tmp - 1; i++) tmp[n++] = ctx[i];
    for (const char *s = ": "; *s && n < sizeof tmp - 1; s++) tmp[n++] = *s;
    for (size_t i = 0; i < ml && n < sizeof tmp - 1; i++) tmp[n++] = e.msg[i];
    tmp[n] = '\0';
    memcpy(e.msg, tmp, n + 1);
    return e;
}

#define RESULT(T) struct { bool ok; union { T val; Error err; }; }

#define OK(RT, v)        ((RT){ .ok = true, .val = (v) })
#define ERR(RT, e)       ((RT){ .ok = false, .err = (e) })
#define ERRF(RT, k, ...) ERR(RT, error_make((k), __VA_ARGS__))

/* `expr?` : yield the value, or return the error from the enclosing function. */
#define TRY(RT, expr) ({                                  \
    __typeof__(expr) _try_r = (expr);                     \
    if (!_try_r.ok) return ERR(RT, _try_r.err);           \
    _try_r.val; })

/* Same, but add context to the error on the way out. */
#define TRY_CTX(RT, expr, ctx) ({                                         \
    __typeof__(expr) _try_r = (expr);                                     \
    if (!_try_r.ok) return ERR(RT, error_context(_try_r.err, (ctx)));     \
    _try_r.val; })

/* `expr?;` for Result<()> -- no value to yield. */
#define TRY0(RT, expr) do {                               \
    __typeof__(expr) _try_r = (expr);                     \
    if (!_try_r.ok) return ERR(RT, _try_r.err);           \
} while (0)

typedef RESULT(char) ResultUnit;
#define OK_UNIT(RT) OK(RT, 0)

#endif
