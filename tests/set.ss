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
