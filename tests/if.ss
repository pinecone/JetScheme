;; Conditionals

;; Basic if
($check (= 1 (if #t 1 2)))
($check (= 2 (if #f 1 2)))

;; Truthy values -- everything except #f is true
($check (= 1 (if 0 1 2)))
($check (= 1 (if "" 1 2)))
($check (= 1 (if '() 1 2)))
($check (= 1 (if 42 1 2)))

;; Nested if
($check (= 3 (if #t (if #f 1 3) 2)))
($check (eq? 'c (if #f 'a (if #f 'b 'c))))

;; If in tail position
(define (abs-val x)
  (if (< x 0) (- x) x))
($check (= 5 (abs-val -5)))
($check (= 5 (abs-val 5)))
($check (= 0 (abs-val 0)))

;; Tail calls in if branches (was a bug in the old compiler)
(define (even? n)
  (if (= n 0) #t
      (odd? (- n 1))))
(define (odd? n)
  (if (= n 0) #f
      (even? (- n 1))))
($check (even? 1000))
($check (odd? 999))
($check (not (even? 999)))
($check (not (odd? 1000)))
