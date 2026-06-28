;; Inliner correctness

;; Basic single- and multi-arg inlining
(define (sq x) (* x x))
(define (sum-sq a b) (+ (sq a) (sq b)))
($check (= 25 (sum-sq 3 4)))
($check (= 9 (sq 3)))

;; Multi-level: sum-sq inlines sq; both reachable from one call
($check (= 100 (sum-sq 6 8)))

;; Self-recursion: the recursive call must stay a call, not inline forever
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
($check (= 120 (fact 5)))
($check (= 3628800 (fact 10)))

;; Mutual recursion
(define (my-even? n) (if (= n 0) #t (my-odd? (- n 1))))
(define (my-odd? n) (if (= n 0) #f (my-even? (- n 1))))
($check (my-even? 10))
($check (my-odd? 7))
($check (my-even? 0))

;; Tail position must survive inlining: a large loop must not grow the stack
(define (loop i acc) (if (= i 0) acc (loop (- i 1) (+ acc 1))))
($check (= 1000000 (loop 1000000 0)))

;; Hygiene: an inlined body's free variable is a global; a same-named local at
;; the call site must NOT capture it.
(define g 100)
(define (uses-g x) (+ x g))
(define (caller g) (uses-g 5))
($check (= 105 (caller 999)))

;; Hygiene through a let-introduced shadow (let desugars to an inlined lambda)
(define base 1000)
(define (add-base n) (+ n base))
(define (compute) (let ((base 1)) (add-base 50)))
($check (= 1050 (compute)))

;; Constant folding: a top-level constant used inside an inlined function
(define K 42)
(define (add-k x) (+ x K))
($check (= 50 (add-k 8)))

;; Constant hygiene: a local named like the constant is not folded away
(define (shadow-k K) (add-k K))
($check (= 1042 (shadow-k 1000)))

;; Arguments are evaluated exactly once, even when a parameter is used twice
(define counter 0)
(define (bump) (set! counter (+ counter 1)) counter)
(define (twice x) (+ x x))
($check (= 2 (twice (bump))))
($check (= 1 counter))

;; A candidate used as a value (not called) still works
(define (dbl x) (* x 2))
($check (equal? '(2 4 6 8) (map dbl (list 1 2 3 4))))
