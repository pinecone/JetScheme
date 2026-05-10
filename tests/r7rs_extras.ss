(define p (cons 1 2))
(set-cdr! p 99)
($check (= 99 (cdr p)))

($check (boolean=? #t #t))
($check (not (boolean=? #t #f)))

($check (symbol=? 'a 'a))
($check (not (symbol=? 'a 'b)))

($check (even? 4))
($check (odd? 5))
($check (not (even? 5)))

($check (procedure? car))
($check (procedure? (lambda (x) x)))
($check (not (procedure? 1)))
