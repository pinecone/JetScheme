# CityScheme

A Scheme interpreter written in C++ that tries to go fast.

See **[pinecone.github.io/CityScheme](https://pinecone.github.io/CityScheme/)** for features, build instructions, cross-language benchmarks, and notes on the implementation techniques.

Licensed under the [MIT License](LICENSE).

---

# CITY(1)

## NAME

city - fast Scheme interpreter

## SYNOPSIS

**city** *file.ss* \[*args*...\]  
**city run** *file.ss* \[*args*...\]  
**city compile** \[*file.ss* | **-**\]  
**city exec** \[*file.bc* | **-**\] \[*args*...\]  
**city disasm** \[*file.ss* | *file.bc* | **-**\]

## DESCRIPTION

**city** compiles and runs Scheme source. The language tracks R7RS-small.

## COMMANDS

**run** *file.ss*
:   Compile and execute in one step. The bare form **city** *file.ss* is shorthand.

**compile** \[*file.ss* | **-**\]
:   Compile source to bytecode on stdout. **-** reads from stdin.

**exec** \[*file.bc* | **-**\]
:   Execute pre-compiled bytecode. **-** reads from stdin.

**disasm** \[*file.ss* | *file.bc* | **-**\]
:   Compile (if source) and disassemble.

## OPTIONS

**--no-prelude**
:   Skip the bundled prelude.

**--no-inline**
:   Disable the function inliner.

**--trace**
:   Print one trace line per opcode dispatched. Debug build only.

## DEVIATIONS FROM R7RS-SMALL

**All numbers are 64-bit doubles.** No exact/inexact tower, no rationals, no bignums. `exact?` is true for integral doubles. `integer?` likewise.

**Strings and characters are byte-level.** Storage is UTF-8; `string-length`, `string-ref`, `string-set!`, `char->integer` are byte-indexed. Bytewise compare gives correct codepoint order on valid UTF-8. Char predicates, `char-upcase`/`-downcase`, `string-upcase`/`-downcase` are ASCII-only.

## EXTRAS

### SRFI 1 procedures

`first`, `second`, `third`, `fourth`, `fifth`, `take`, `drop`, `last`, `concatenate`, `fold`, `fold-right`, `reduce-right`, `filter`.

### SRFI 60 procedures

`bitwise-and`, `bitwise-ior`, `bitwise-xor`, `bitwise-not`, `arithmetic-shift`. Operate on integral doubles.

### struct, isa?

```scheme
(define point (struct 'point '(x y)))     ; create a fresh type
(define p (point 3 4))                    ; call the type to construct
(isa? p point)                            ; => #t
(ref p 'x)                                ; => 3
(setf! (ref p 'x) 10)                     ; mutate field
```

Each `(struct ...)` call yields a distinct type, even when invoked twice with the same name and field list.

### ref

`(ref v i)` is a polymorphic accessor. It works on vectors and strings (integer index) and structs (symbol field name).

### setf!

Mutates the place denoted by its first argument:

```scheme
(setf! (ref v 3) 'x)             ; vector-set!
(setf! (ref s 0) #\H)            ; string-set!
(setf! (ref pt 'x) 99)           ; struct field mutation
```

### $check

`($check expr)` evaluates *expr* and, if false, prints the source location and exits. Used as a test assertion.

### argv

Vector of strings holding the script's command-line arguments (excluding the binary and script path).

### Random

`(random)` returns a pseudorandom non-negative integer. `(random-seed n)` seeds the generator with *n*.

### Conveniences

`displayn` is `display` followed by `newline`. `rest` is an alias for `cdr`.

## ENVIRONMENT

**CITY_PRELUDE** overrides the prelude path. Default: resolved relative to the binary.

## FILES

*lib/prelude.ss* is the bundled prelude, auto-loaded unless `--no-prelude` is given.

## SEE ALSO

pinecone.github.io/CityScheme
