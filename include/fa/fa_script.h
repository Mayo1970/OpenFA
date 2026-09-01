/*
 * fa_script.h - Lua 4.0 assignment-subset evaluator (RRR-36)
 *
 * Parity basis: RRR-11 (PL-039, PL-040) and ENGINE-ARCH section 10. The
 * original runs `GData\Scripts\*.jrs` through `C_LuaScript::DoFile`, which
 * loads each file as a bare Lua 4.0 chunk with NO standard library, NO host
 * callbacks and NO `DoString` / `DoBuffer` path (both are dead code). The
 * engine then reads the globals the chunk assigned.
 *
 * RRR-11 confirmed the reachable surface is tiny and frozen: 130 shipped
 * `.jrs` files, every line a single `NAME = value` assignment, value being a
 * string literal, an integer literal, or a reference to an earlier global
 * (`MAP_WIDTH = FULLSCREEN_WIDTH`). No control flow, no functions, no table
 * constructors, no comments, no arithmetic. The animated-object scripts use a
 * separate hand-written parser (RRR-21), not this path.
 *
 * DECISION (RRR-36, open item 3 in ENGINE-ARCH section 14): a purpose-built
 * evaluator, not a vendored interpreter. It matches Lua 4.0 EXACTLY for the
 * constructs the acceptance criteria name - number parsing, string escapes,
 * table constructors and variable references - and refuses anything outside
 * that subset with a line-numbered error. The core links zero external
 * libraries (ENGINE-ARCH section 13); a vendored Lua would break that and add
 * ~6k lines for a grammar the corpus never exercises. If future content needs
 * more of Lua 4.0, this is the seam to swap.
 *
 * Grammar accepted (a strict subset of Lua 4.0):
 *
 *     chunk  := {stat}
 *     stat   := NAME '=' exp [';']
 *     exp    := {'-' | '+'} operand                 (unary, on numbers)
 *     operand:= NUMBER | STRING | table | NAME | '(' exp ')'
 *     table  := '{' [field {fieldsep field} [fieldsep]] '}'
 *     field  := '[' exp ']' '=' exp | NAME '=' exp | exp
 *     fieldsep := ',' | ';'
 *
 * Lua 4.0 semantics that are reproduced:
 *   - numbers are doubles; the literal grammar is decimal digits with an
 *     optional fraction and an optional [eE][+-]?digits exponent (no hex -
 *     Lua 4.0 has none). fa_script_int() reports the value only when it is
 *     integral.
 *   - string escapes: \a \b \f \n \r \t \v, \<newline>, \ddd (1-3 decimal
 *     digits, <= 255), and \<any other char> which yields that char verbatim
 *     (so "\\" is a backslash and "\0" is a single NUL byte, matching
 *     EngineInit.jrs). Strings are stored with an explicit length and are
 *     NUL-safe. A raw newline inside a string is an error.
 *   - reading an unassigned global yields nil.
 *   - `--` starts a line comment (accepted though the corpus has none).
 *
 * Simplifications (documented, none observable for the read-only consumers):
 *   - tables are values, not references: `b = a` deep-copies. Scripts never
 *     mutate a table after building it, so this is indistinguishable.
 *   - assignment targets are a single NAME (no `a.b` / `a[c]` lvalues).
 *   - no binary operators, `..`, comparisons, `and`/`or`, `not`, function
 *     calls or field access. Each is a line-numbered "outside the .jrs
 *     subset" error, not silent acceptance.
 */
#ifndef FA_SCRIPT_H
#define FA_SCRIPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fa_script fa_script;

typedef enum {
    FA_SCRIPT_NIL = 0,
    FA_SCRIPT_NUMBER,
    FA_SCRIPT_STRING,
    FA_SCRIPT_TABLE
} fa_script_type;

/* Create an evaluator with an empty global environment. NULL on OOM. */
fa_script *fa_script_new(void);

/* Destroy an evaluator and everything it holds. */
void fa_script_free(fa_script *s);

/*
 * Evaluate one chunk. `src` need not be NUL-terminated; `len` is its length.
 * `chunkname` is used in error messages (for example the file name); NULL
 * becomes "?". Globals persist in `s` and are visible to a later
 * fa_script_do call and to the getters below.
 *
 * Returns 0 on success, -1 on any lex / parse / evaluation error. On -1,
 * fa_script_last_error(s) returns "chunkname:line: message". Globals assigned
 * before the failing statement keep their values.
 */
int fa_script_do(fa_script *s, const char *src, size_t len,
                 const char *chunkname);

/* Convenience: read a whole file and evaluate it. Returns 0, -1 on an I/O
 * error (message in fa_script_last_error) or -1 from fa_script_do. */
int fa_script_do_file(fa_script *s, const char *path);

const char *fa_script_last_error(const fa_script *s);

/* --- global lookup ------------------------------------------------- */

fa_script_type fa_script_type_of(const fa_script *s, const char *name);

/* Number value of a global. Returns 0 and writes *out on success; -1 if the
 * global is absent or not a number. */
int fa_script_number(const fa_script *s, const char *name, double *out);

/* Integer value of a global. Returns 0 and writes *out only when the global
 * is a number with no fractional part and in long range; -1 otherwise. */
int fa_script_int(const fa_script *s, const char *name, long *out);

/*
 * String value of a global. Returns the bytes (NUL-safe: use *len_out for the
 * true length; the buffer is also NUL-terminated for convenience) or NULL if
 * the global is absent or not a string. Valid until fa_script_free. len_out
 * may be NULL.
 */
const char *fa_script_string(const fa_script *s, const char *name,
                             size_t *len_out);

/*
 * Canonical text form of a global's value, for tests and the RRR-24
 * inspector: `nil`, an integer or `%.14g` number, a double-quoted string with
 * \ escapes, or `{ v1, v2, name=v, [k]=v }` in insertion order. Writes at
 * most `cap` bytes including the NUL. Returns the length that would be
 * written (as snprintf does), or -1 if the global is absent.
 */
int fa_script_repr(const fa_script *s, const char *name, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FA_SCRIPT_H */
