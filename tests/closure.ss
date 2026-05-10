;; Closures
(define (make-adder x)
  (lambda (y) (+ y x)))

(define add4 (make-adder 4))
(define add5 (make-adder 5))
(define add6 (make-adder 6))

($check (= 9 (add4 5)))
($check (= 10 (add5 5)))
($check (= 11 (add6 5)))
