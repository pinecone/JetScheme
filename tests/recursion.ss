;; Recursion patterns

;; Fibonacci
(define (fib n)
  (if (< n 2) n
      (+ (fib (- n 1)) (fib (- n 2)))))
($check (= 0 (fib 0)))
($check (= 1 (fib 1)))
($check (= 1 (fib 2)))
($check (= 55 (fib 10)))

;; Factorial
(define (fact n)
  (if (= n 0) 1 (* n (fact (- n 1)))))
($check (= 1 (fact 0)))
($check (= 120 (fact 5)))

;; Ackermann (small values)
(define (ack m n)
  (if (= m 0) (+ n 1)
      (if (= n 0) (ack (- m 1) 1)
          (ack (- m 1) (ack m (- n 1))))))
($check (= 7 (ack 2 2)))
($check (= 61 (ack 3 3)))

;; Mutual recursion
(define (is-even n)
  (if (= n 0) #t (is-odd (- n 1))))
(define (is-odd n)
  (if (= n 0) #f (is-even (- n 1))))
($check (is-even 100))
($check (is-odd 99))
($check (not (is-even 99)))

;; Map and fold
($check (equal? '(2 4 6) (map (lambda (x) (* x 2)) '(1 2 3))))
($check (= 15 (fold + 0 '(1 2 3 4 5))))

;; Deep tail recursion
(define (loop n) (if (= n 0) 'done (loop (- n 1))))
($check (eq? 'done (loop 50000)))
