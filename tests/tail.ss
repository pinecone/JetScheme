;; Tail call optimization -- would stack overflow without TCO
(define (count n)
  (if (zero? n) n
      (count (- n 1))))

($check (= 0 (count 2500)))
