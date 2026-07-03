;; Mutation with set!

(define a 5)
($check (= 5 a))

(set! a 6)
($check (= 6 a))

(define b a)
($check (= 6 b))

(set! a 7)
($check (= 6 b))  ;; b is not affected
($check (= 7 a))

;; set! with lists
(set! a '(1 2 3))
(set! b a)
(set! a '(1 2 3 4))
($check (equal? '(1 2 3 4) a))
($check (equal? '(1 2 3) b))

;; set-car!
(set! a '(1 2 3))
(set-car! a 2)
($check (equal? '(2 2 3) a))

;; set! with a complex RHS: the value is computed via an ANF temp, then stored.
(define sc 1)
(set! sc (+ sc (* 2 (+ sc 2))))
($check (= 7 sc))
(set! sc (if (> sc 5) (begin (set! sc 100) (+ sc 1)) 'small))
($check (= 101 sc))

;; set! of a captured+mutated (boxed) local with a complex RHS.
(define (make-acc)
  (let ((total 0))
    (lambda (n) (set! total (+ total n)) total)))
(define acc (make-acc))
(acc 3)
($check (= 10 (acc 7)))
