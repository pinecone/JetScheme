;; fib: tree recursion, non-tail calls, mixed arithmetic + branching.
;; fib(35) = 9227465; ~30M function calls.

(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))

(displayn (fib 35))
