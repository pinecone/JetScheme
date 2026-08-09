;; Deep non-tail recursion grows the frame stack and crosses the stack watermark.
(define (depth-sum n)
  (if (= n 0)
      0
      (+ n (depth-sum (- n 1)))))
($check (= 3200040000 (depth-sum 80000)))

;; GC pressure with live roots: allocate far past gc_threshold while holding a long list.
(define (build n acc)
  (if (= n 0)
      acc
      (build (- n 1) (cons (vector n n n) acc))))
(define live (build 50000 '()))
($check (= 50000 (length live)))
($check (= 1 (vector-ref (car live) 0)))
($check (= 50000 (vector-ref (car (reverse live)) 0)))

;; Garbage churn with the survivors still live
(define (churn n)
  (if (= n 0)
      'ok
      (begin (make-vector 100 n) (churn (- n 1)))))
($check (eq? 'ok (churn 20000)))
($check (= 50000 (length live)))
