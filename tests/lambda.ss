;; Lambda and closures

;; Basic lambda
($check (= 3 ((lambda (x) x) 3)))
($check (= 7 ((lambda (x y) (+ x y)) 3 4)))

;; Closure captures
(define (make-counter)
  (define n 0)
  (lambda ()
    (set! n (+ n 1))
    n))

(define c (make-counter))
($check (= 1 (c)))
($check (= 2 (c)))
($check (= 3 (c)))

;; Independent counters
(define c2 (make-counter))
($check (= 1 (c2)))
($check (= 4 (c)))

;; Variadic lambda
(define va (lambda args args))
($check (equal? '(1 2 3) (va 1 2 3)))
($check (null? (va)))

;; Higher-order functions
(define (compose f g) (lambda (x) (f (g x))))
(define inc (lambda (x) (+ x 1)))
(define dbl (lambda (x) (* x 2)))
($check (= 7 ((compose inc dbl) 3)))   ;; (+ (* 3 2) 1)
($check (= 8 ((compose dbl inc) 3)))   ;; (* (+ 3 1) 2)

;; Nested closures
(define (make-adder-maker)
  (lambda (x)
    (lambda (y) (+ x y))))
(define add10 ((make-adder-maker) 10))
($check (= 15 (add10 5)))

;; User-written variadic IIFE: stays a real closure call (never contracted).
($check (equal? '(1 2 3) ((lambda args args) 1 2 3)))
($check (= 6 (apply + ((lambda args args) 1 2 3))))
($check (null? ((lambda args args))))

;; Arity-matched IIFEs contract to lets, including nested and shadowing forms.
($check (= 11 ((lambda (x) ((lambda (x) (+ x 1)) (* x 2))) 5)))
($check (= 42 ((lambda () 42))))

;; Closure capture: basic make-adder
(define (make-adder x)
  (lambda (y) (+ y x)))

(define add4 (make-adder 4))
(define add5 (make-adder 5))
(define add6 (make-adder 6))

($check (= 9 (add4 5)))
($check (= 10 (add5 5)))
($check (= 11 (add6 5)))

;; Closures: shared mutable state between siblings, deep nesting,
;; per-call independence.

;; Two siblings sharing one mutable variable.
(define (make-pair)
  (define n 0)
  (cons (lambda () n)
        (lambda (v) (set! n v))))
(define p (make-pair))
($check (= 0 ((car p))))
((cdr p) 42)
($check (= 42 ((car p))))
((cdr p) -7)
($check (= -7 ((car p))))

;; Outer reader observing inner mutation.
(define (foo)
  (define n 0)
  (define inc (lambda () (set! n (+ n 1))))
  (inc) (inc) (inc)
  n)
($check (= 3 (foo)))

;; Independent invocations get independent state.
(define p1 (make-pair))
(define p2 (make-pair))
((cdr p1) 100)
((cdr p2) 200)
($check (= 100 ((car p1))))
($check (= 200 ((car p2))))

;; Three-level nesting: innermost mutates a variable owned two frames out;
;; the (get . set) pair is built from the deepest scope so reads don't mutate.
(define (triple)
  (define x 1)
  (lambda ()
    (cons (lambda () x)
          (lambda (v) (set! x v)))))
(define t1 ((triple)))
((cdr t1) 7)
($check (= 7 ((car t1))))

;; Separate triple invocation: its own x.
(define t2 ((triple)))
((cdr t2) 99)
($check (= 99 ((car t2))))
($check (= 7 ((car t1))))   ;; t1's x untouched by t2

;; Inner closure capturing both an enclosing param and a toplevel binding.
(define multiplier 10)
(define (scale by)
  (lambda (x) (* multiplier (+ x by))))
(define s5 (scale 5))
($check (= 80 (s5 3)))    ;; 10 * (3 + 5)
(set! multiplier 2)
($check (= 16 (s5 3)))    ;; toplevel mutation visible: 2 * (3 + 5)
