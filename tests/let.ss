;; Let expressions

;; Basic let
(define r1 (let ((x 1) (y 2)) (+ x y)))
($check (= 3 r1))

;; Nested let
(define r2 (let ((x 1))
             (let ((y 2) (z 3))
               (+ x y z))))
($check (= 6 r2))

;; Let bindings are parallel -- y sees outer x
(define x 10)
(define r3 (let ((y x)) (+ 1 y)))
($check (= 11 r3))

;; Let with body sequence
(define r4 (let ((a 1))
             (define b 2)
             (+ a b)))
($check (= 3 r4))

;; Shadowing
(define z 100)
(define r5 (let ((z 5)) z))
($check (= 5 r5))
($check (= 100 z))
