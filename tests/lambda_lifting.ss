;; Lambda lifting / closure capture cases.

;; Inline-applied lambda with an internal define.
($check (= 10 ((lambda (x) (define y (* x 2)) y) 5)))
;; Inline-applied variadic lambda.
($check (equal? '(1 2 3) ((lambda args args) 1 2 3)))
($check (= 10 ((lambda args (apply + args)) 1 2 3 4)))

;; set! on own param (no capture).
($check (= 42 ((lambda (x) (set! x 42) x) 1)))
(define (mut x) (set! x (* x 2)) x)
($check (= 10 (mut 5)))

;; set! on a captured variable: each counter keeps its own state across calls.
(define (make-c init)
  (lambda ()
    (set! init (+ init 1))
    init))
(define c1 (make-c 0))
($check (= 1 (c1)))
($check (= 2 (c1)))
($check (= 3 (c1)))
;; Independent counters share no state.
(define c2 (make-c 100))
($check (= 101 (c2)))
($check (= 4 (c1)))
($check (= 102 (c2)))

;; Closed inner lambda passed to map.
(define (squares-of lst) (map (lambda (x) (* x x)) lst))
($check (equal? '(1 4 9 16) (squares-of '(1 2 3 4))))

;; apply on a closure that captures.
(define (cons-x x) (lambda args (cons x args)))
($check (equal? '(a 1 2) (apply (cons-x 'a) '(1 2))))

;; Computed callee (not a direct symbol reference).
(define (pick t) (if t + -))
($check (= 8 ((pick #t) 5 3)))
($check (= 2 ((pick #f) 5 3)))
