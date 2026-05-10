;; Reader: parse datums from a separate data file via `read`.

(define p (open-input-file "reader_data.txt"))

;; numbers (decimal int, signed, float, signed float, hex, signed hex)
($check (= 42 (read p)))
($check (= -7 (read p)))
($check (= 1 (read p)))
($check (= 3.14 (read p)))
($check (= -3.14 (read p)))
($check (= 255 (read p)))
($check (= -31 (read p)))

;; symbols (peculiar +, -, multi-char with special chars)
($check (eq? 'foo (read p)))
($check (eq? '+ (read p)))
($check (eq? '- (read p)))
($check (eq? '<= (read p)))
($check (eq? 'string? (read p)))
($check (eq? 'set! (read p)))

;; booleans
($check (eq? #t (read p)))
($check (eq? #f (read p)))

;; characters (incl. ')')
($check (eqv? #\a (read p)))
($check (eqv? #\) (read p)))

;; strings (empty, plain, escapes, embedded delimiters)
($check (equal? "" (read p)))
($check (equal? "hello" (read p)))
($check (equal? "with\nnewline\ttab\\bs\"q" (read p)))
($check (equal? "parens(and)semis;inside" (read p)))

;; empty list
($check (null? (read p)))

;; simple list (1 2 3)
(define a (read p))
($check (= 1 (car a)))
($check (= 2 (car (cdr a))))
($check (= 3 (car (cdr (cdr a)))))
($check (null? (cdr (cdr (cdr a)))))

;; nested list (a (b c) d)
(define b (read p))
($check (eq? 'a (car b)))
($check (eq? 'b (car (car (cdr b)))))
($check (eq? 'c (car (cdr (car (cdr b))))))
($check (eq? 'd (car (cdr (cdr b)))))

;; mid-list comment: (a ; ...\n b) -> (a b)
(define mc (read p))
($check (eq? 'a (car mc)))
($check (eq? 'b (car (cdr mc))))
($check (null? (cdr (cdr mc))))

;; vector #(1 2 3)
(define v (read p))
($check (vector? v))
($check (= 3 (vector-length v)))
($check (= 1 (vector-ref v 0)))
($check (= 2 (vector-ref v 1)))
($check (= 3 (vector-ref v 2)))

;; empty vector #()
(define ev (read p))
($check (vector? ev))
($check (= 0 (vector-length ev)))

;; nested vector #(#(1 2) #(3 4))
(define nv (read p))
($check (vector? nv))
($check (= 2 (vector-length nv)))
($check (vector? (vector-ref nv 0)))
($check (= 1 (vector-ref (vector-ref nv 0) 0)))
($check (= 4 (vector-ref (vector-ref nv 1) 1)))

;; quote: 'foo -> (quote foo)
(define q (read p))
($check (eq? 'quote (car q)))
($check (eq? 'foo (car (cdr q))))
($check (null? (cdr (cdr q))))

;; double quote: ''double -> (quote (quote double))
(define qq (read p))
($check (eq? 'quote (car qq)))
($check (eq? 'quote (car (car (cdr qq)))))
($check (eq? 'double (car (cdr (car (cdr qq))))))

;; quote of list: '(1 2 3) -> (quote (1 2 3))
(define ql (read p))
($check (eq? 'quote (car ql)))
($check (= 1 (car (car (cdr ql)))))
($check (= 3 (car (cdr (cdr (car (cdr ql)))))))

;; nested quote: '(a 'nested-quote) -> (quote (a (quote nested-quote)))
(define nq (read p))
($check (eq? 'quote (car nq)))
(define inner (car (cdr nq)))
($check (eq? 'a (car inner)))
($check (eq? 'quote (car (car (cdr inner)))))
($check (eq? 'nested-quote (car (cdr (car (cdr inner))))))

;; quote of vector: '#(1 2) -> (quote #(1 2))
(define qv (read p))
($check (eq? 'quote (car qv)))
($check (vector? (car (cdr qv))))
($check (= 1 (vector-ref (car (cdr qv)) 0)))

;; vector with quoted elements: #('a 'b)
(define vq (read p))
($check (vector? vq))
($check (= 2 (vector-length vq)))
($check (eq? 'quote (car (vector-ref vq 0))))
($check (eq? 'a (car (cdr (vector-ref vq 0)))))
($check (eq? 'b (car (cdr (vector-ref vq 1)))))

;; deep nesting
(define deep (read p))
($check (eq? 'deep (car (car (car (car (car deep)))))))

;; trailing whitespace and comments still yield atom
($check (= 99 (read p)))

;; eof
($check (eof-object? (read p)))
($check (eof-object? (read p)))

(close-input-port p)
