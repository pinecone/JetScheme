;; Vector operations

(define a '#(1 2 3))
($check (= 1 (vector-ref a 0)))
($check (= 2 (vector-ref a 1)))
($check (equal? '#(1 2 3) a))

($check (= 9 (vector-ref (vector 9 'hobo) 0)))
($check (eq? 'hobo (vector-ref (vector 'hobo 9) 0)))
($check (eq? 'hobo (vector-ref (vector 9 'hobo) 1)))
($check (= 9 (vector-ref (vector 'hobo 9) 1)))

(define b (make-vector 0 4))
(define c (make-vector 4 0))
($check (equal? '#() b))
($check (equal? '#(0 0 0 0) c))
($check (equal? '#(1 2 3 4 #(1 2 3 4) 5) '#(1 2 3 4 #(1 2 3 4) 5)))

;; eqv?/eq? on vectors are pointer identity, not value.
($check (not (eqv? '#(1 2 3) '#(1 2 3))))
($check (not (eq? '#(1 2 3) '#(1 2 3))))
(define same '#(1 2 3))
($check (eqv? same same))
($check (eq? same same))

;; ref / (setf! (ref ...) ...) on vectors.

(define rv (vector 10 20 30 40))
($check (= 10 (ref rv 0)))
($check (= 20 (ref rv 1)))
($check (= 40 (ref rv 3)))
($check (= 40 (ref rv (- 4 1))))

(setf! (ref rv 1) 999)
($check (= 999 (ref rv 1)))
;; vector-ref agrees.
($check (= 999 (vector-ref rv 1)))

(setf! (ref rv 0) 'sym)
($check (eq? 'sym (ref rv 0)))

;; the set! form returns the assigned value.
($check (= 7 (setf! (ref rv 2) 7)))
($check (= 7 (ref rv 2)))

;; hot loop.
(define hot (make-vector 100 0))
(define i 0)
(let loop ()
  (when (< i 100)
    (setf! (ref hot i) (* i i))
    (set! i (+ i 1))
    (loop)))
(define sum 0)
(set! i 0)
(let loop ()
  (when (< i 100)
    (set! sum (+ sum (ref hot i)))
    (set! i (+ i 1))
    (loop)))
;; sum of i*i for i in [0..100) = 328350
($check (= 328350 sum))

;; (apply ref ...) routes through the slow primitive fallback.
($check (= 999 (apply ref (list rv 1))))

;; locally shadowed `ref`: the let-bound function wins, not the builtin.
($check (= 42 (let ((ref (lambda (v i) 42))) (ref rv 0))))
