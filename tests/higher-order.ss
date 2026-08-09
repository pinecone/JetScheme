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


;; Apply

($check (= 15 (apply + '(1 2 3 4 5))))

(define (mul-n lis) (apply * lis))
($check (= 120 (mul-n '(1 2 3 4 5))))

(define (thunk) 'thunk-ok)
($check (eq? 'thunk-ok (apply thunk '())))

;; N-ary via apply
(define n-ary-thing (lambda args args))
($check (equal? '(1 2 3) (apply n-ary-thing '(1 2 3))))

;; fold-right keeps list order; fold reverses it
($check (equal? '(1 2 3) (fold-right cons '() '(1 2 3))))
($check (= 6 (fold-right + 0 '(1 2 3))))
($check (equal? '(1 (2 (3 ()))) (fold-right list '() '(1 2 3))))

;; reduce-right uses the identity only for the empty list
($check (= 0 (reduce-right + 0 '())))
($check (= 5 (reduce-right + 0 '(5))))
($check (= 6 (reduce-right + 0 '(1 2 3))))
($check (equal? '(1 2 . 3) (reduce-right cons 'unused '(1 2 3))))
