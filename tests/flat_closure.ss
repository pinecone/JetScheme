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
