;; begin: evaluate body forms in order, return the value of the last.

;; Single-expression begin returns that expression's value.
($check (= 1 (begin 1)))
($check (eq? 'x (begin 'x)))

;; Multiple expressions: last value wins.
($check (= 3 (begin 1 2 3)))
($check (eq? 'c (begin 'a 'b 'c)))

;; Side effects fire in order, left to right.
(define x 0)
(begin (set! x 1) (set! x (+ x 10)) (set! x (* x 2)))
($check (= 22 x))

;; The value of the begin is the last expression's value, even with side effects.
(set! x 0)
($check (= 99 (begin (set! x 1) (set! x 2) 99)))
($check (= 2 x))

;; All intermediate values are discarded; only the last is observed.
($check (= 7 (begin (+ 1 1) (+ 2 2) (+ 3 4))))

;; Nested begins flatten semantically -- inner last value bubbles out.
($check (= 5 (begin (begin 1 2) (begin 3 4 5))))
($check (= 9 (begin (begin (begin 9)))))

;; begin inside lambda body (this is the implicit-begin shape too, but explicit
;; begin must work the same).
(define (f)
  (begin
    (set! x (+ x 1))
    (set! x (+ x 1))
    x))
(set! x 0)
($check (= 2 (f)))
($check (= 2 x))

;; begin as the consequent of an if.
(set! x 0)
(if #t (begin (set! x 10) (set! x (+ x 5))) (set! x -1))
($check (= 15 x))

;; begin as the alternate of an if.
(set! x 0)
(if #f (set! x -1) (begin (set! x 100) (set! x (+ x 1))))
($check (= 101 x))

;; begin in tail position: 50k-deep recursion must not blow the stack.
;; The recursive call is the last form of the begin, so it inherits tail.
(define bn 0)
(define (bloop n)
  (begin
    (set! bn (+ bn 1))
    (if (zero? n) 'done (bloop (- n 1)))))
($check (eq? 'done (bloop 50000)))
($check (= 50001 bn))

;; Top-level begin splices: defines inside it are visible afterwards.
(begin
  (define top-a 11)
  (define top-b 22))
($check (= 11 top-a))
($check (= 22 top-b))
($check (= 33 (+ top-a top-b)))

;; Order of evaluation: capture intermediates into a list to verify.
(define trace '())
(begin
  (set! trace (cons 'one trace))
  (set! trace (cons 'two trace))
  (set! trace (cons 'three trace)))
($check (equal? '(three two one) trace))

(displayn "begin ok")
