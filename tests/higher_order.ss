;; Higher-order functions from the standard library

;; map
($check (equal? '(1 4 9) (map (lambda (x) (* x x)) '(1 2 3))))
($check (null? (map (lambda (x) x) '())))

;; filter
($check (equal? '(2 4 6) (filter (lambda (x) (= 0 (modulo x 2))) '(1 2 3 4 5 6))))
($check (null? (filter (lambda (x) #f) '(1 2 3))))
($check (equal? '(1 2 3) (filter (lambda (x) #t) '(1 2 3))))

;; fold
($check (= 15 (fold + 0 '(1 2 3 4 5))))
($check (= 120 (fold * 1 '(1 2 3 4 5))))
($check (equal? '(3 2 1) (fold cons '() '(1 2 3))))

;; for-each
(define result '())
(for-each (lambda (x) (set! result (cons x result))) '(1 2 3))
($check (equal? '(3 2 1) result))

