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

;; Inner let val is evaluated in the enclosing scope, before the inner name binds.
($check (= 25 (let ((a 5)) (let ((b (* a a))) b))))
($check (= 40 (let ((a 10)) (let ((a 20)) (+ a a)))))

;; A let-bound name that shadows a primitive wins
($check (= 42 (let ((ref (lambda (v i) 42))) (ref (vector 7 8) 0))))
($check (= 12 (let ((+ (lambda (a b) (* a b)))) (+ 3 4))))
($check (eq? 'shadowed (let ((< (lambda (a b) 'shadowed))) (< 1 2))))

;; A let-bound name that is the primitive still lowers.
($check (= 8 (let ((ref (%prim "ref"))) (ref (vector 5 8) 1))))

;; let-bound variable captured and mutated by a closure -> boxed frame slot.
(define (make-counter)
  (let ((n 0))
    (lambda () (set! n (+ n 1)) n)))
(define ctr (make-counter))
($check (= 1 (ctr)))
($check (= 2 (ctr)))
($check (= 3 (ctr)))

;; let* binds sequentially: each val sees the previous names.
($check (= 6 (let* ((a 1) (b (+ a 1)) (c (+ b 1))) (+ a b c))))
($check (= 5 (let* ((a 2) (a (+ a a)) (a (+ a 1))) a)))
