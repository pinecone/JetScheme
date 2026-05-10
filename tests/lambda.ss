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
