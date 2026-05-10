;; Apply

($check (= 15 (apply + '(1 2 3 4 5))))

(define (mul-n lis) (apply * lis))
($check (= 120 (mul-n '(1 2 3 4 5))))

(define (thunk) 'thunk-ok)
($check (eq? 'thunk-ok (apply thunk '())))

;; N-ary via apply
(define n-ary-thing (lambda args args))
($check (equal? '(1 2 3) (apply n-ary-thing '(1 2 3))))
