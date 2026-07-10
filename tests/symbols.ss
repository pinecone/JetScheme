;; Symbol operations

($check (equal? "bar" (symbol->string 'bar)))
($check (eq? 'bar (string->symbol "bar")))
($check (eq? 'bar 'bar))
($check (not (eq? 'bar 'foo)))

(define bar 'bar)
($check (eq? bar 'bar))
