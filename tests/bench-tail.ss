;; Tail call benchmark - deep tail recursion, no output until done
(define (loop n)
  (if (zero? n)
      n
      (loop (- n 1))))

(displayn (loop 500000))
