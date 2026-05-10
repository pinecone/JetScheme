;; Fibonacci benchmark - exponential recursion, heavy on calls and arithmetic
(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))

(displayn (fib 24))
