;; Named let.

;; Classic counting loop.
(define r1
  (let loop ((i 0) (sum 0))
    (if (= i 10) sum (loop (+ i 1) (+ sum i)))))
($check (= 45 r1))

;; Building a list, accumulator-style.
(define r2
  (let build ((n 5) (acc '()))
    (if (= n 0) acc (build (- n 1) (cons n acc)))))
($check (equal? '(1 2 3 4 5) r2))

;; Deep tail recursion through the named-let loop.
(define r3
  (let go ((n 100000))
    (if (= n 0) 'done (go (- n 1)))))
($check (eq? 'done r3))

;; Body that closes over outer lexicals.
(define limit 7)
(define r4
  (let walk ((i 0))
    (if (= i limit) i (walk (+ i 1)))))
($check (= 7 r4))

;; Named let returning a closure that captures a loop param.
(define adder
  (let make ((n 5))
    (lambda (x) (+ x n))))
($check (= 12 (adder 7)))
