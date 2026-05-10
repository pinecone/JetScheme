;; Letrec.

;; Single self-recursion.
(define r1
  (letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))))
    (fact 6)))
($check (= 720 r1))

;; Mutual recursion.
(define r2
  (letrec ((even? (lambda (n) (if (= n 0) #t (odd?  (- n 1)))))
           (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
    (even? 10)))
($check (eq? #t r2))

(define r3
  (letrec ((even? (lambda (n) (if (= n 0) #t (odd?  (- n 1)))))
           (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
    (odd? 11)))
($check (eq? #t r3))

;; letrec* keyword is accepted and behaves the same.
(define r4
  (letrec* ((a 1)
            (b (+ a 1))
            (c (+ b 1)))
    (+ a b c)))
($check (= 6 r4))

;; Body sees outer bindings normally.
(define outer 100)
(define r5
  (letrec ((f (lambda () outer)))
    (f)))
($check (= 100 r5))

;; Tail calls inside letrec'd procedures don't blow the stack.
(define r6
  (letrec ((loop (lambda (n) (if (= n 0) 'done (loop (- n 1))))))
    (loop 100000)))
($check (eq? 'done r6))
