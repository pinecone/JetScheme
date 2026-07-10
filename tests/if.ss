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

;; cond: clause bodies must run every expression in order, returning the last.

;; Single-expression clauses (the easy case).
($check (= 1 (cond ((= 1 1) 1) (else 2))))
($check (= 2 (cond ((= 1 2) 1) (else 2))))
($check (= 3 (cond ((= 1 2) 1) ((= 3 3) 3) (else 2))))

;; Multi-expression clause bodies: side effects must fire AND last value wins.
(define x 0)
(cond ((= 1 1) (set! x (+ x 10)) (set! x (+ x 1))) (else (set! x -1)))
($check (= 11 x))

;; Else branch with multiple expressions.
(set! x 0)
(cond ((= 1 2) (set! x 99)) (else (set! x (+ x 5)) (set! x (+ x 3))))
($check (= 8 x))

;; Last expression in clause is the cond's value, even with prior side effects.
(set! x 0)
($check (= 7 (cond ((= 1 1) (set! x 99) (+ 3 4)) (else 0))))
($check (= 99 x))

;; Side effects in non-matching clauses must NOT fire.
(set! x 0)
(cond ((= 1 2) (set! x 1) (set! x 2))
      ((= 1 1) (set! x 100))
      (else (set! x 999)))
($check (= 100 x))

;; Multi-expression clause inside a function (lambda body) -- the common
;; "small state machine" pattern.
(define counter 0)
(define (bump n)
  (cond ((> n 0) (set! counter (+ counter n)) (set! counter (* counter 2)) counter)
        (else counter)))
($check (= 6 (bump 3)))    ; (0+3)*2 = 6
($check (= 18 (bump 3)))   ; (6+3)*2 = 18
($check (= 18 (bump 0)))   ; no change

(displayn "cond ok")
