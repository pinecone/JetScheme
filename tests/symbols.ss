;; Symbol operations

($check (equal? "bar" (symbol->string 'bar)))
($check (eq? 'bar (string->symbol "bar")))
($check (eq? 'bar 'bar))
($check (not (eq? 'bar 'foo)))

(define bar 'bar)
($check (eq? bar 'bar))

(define stable-symbol (string->symbol "stable-symbol"))
(define (intern-many n)
  (if (= n 0)
      #t
      (begin
        (string->symbol (number->string n))
        (intern-many (- n 1)))))
(intern-many 1000)
($check (eq? stable-symbol (string->symbol "stable-symbol")))
($check (equal? "stable-symbol" (symbol->string stable-symbol)))
