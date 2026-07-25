;; Quote: symbols, lists, dotted pairs, vectors, forms, atoms

;; symbols
($check (eq? 'foo 'foo))
($check (equal? ''a (list 'quote 'a)))

;; lists and dotted pairs
($check (equal? '(1 2 3) (list 1 2 3)))
($check (equal? '(a . b) (cons 'a 'b)))
($check (null? '()))
($check (equal? '(a "s" 5 #t #\c) (list 'a "s" 5 #t #\c)))

;; keyword symbols are ordinary data inside quote
($check (equal? '(lambda x) (list 'lambda 'x)))
($check (equal? '(a lambda) (list 'a 'lambda)))
($check (equal? '(a (b lambda) c) (list 'a (list 'b 'lambda) 'c)))
($check (equal? '(a quote b) (list 'a 'quote 'b)))
($check (equal? '(a . lambda) (cons 'a 'lambda)))
($check (equal? '((a) lambda) (list (list 'a) 'lambda)))
($check (equal? '('a lambda) (list (list 'quote 'a) 'lambda)))
($check (equal? '("s" lambda) (list "s" 'lambda)))
($check (equal? '(lambda define if set! setf! quote apply let let* letrec letrec* begin when unless cond and or include)
                (list 'lambda 'define 'if 'set! 'setf! 'quote 'apply 'let 'let* 'letrec 'letrec* 'begin 'when 'unless 'cond 'and 'or 'include)))

;; quote as a form
($check (equal? (quote lambda) 'lambda))
($check (equal? (quote 'a) (list 'quote 'a)))
($check (equal? (quote (a lambda)) (list 'a 'lambda)))
($check (equal? (quote (a . b)) (cons 'a 'b)))
($check (equal? (quote (quote (a b))) (list 'quote (list 'a 'b))))
($check (equal? (quote (1 2 3)) (list 1 2 3)))

;; quoted vectors and bytevectors
($check (= 2 (vector-length '#(a lambda))))
($check (eq? 'lambda (ref '#(a lambda) 1)))
($check (equal? '#(1 2 3) (vector 1 2 3)))
($check (equal? '#u8(1 2 3) (bytevector 1 2 3)))
($check (= 0 (vector-length '#())))
($check (= 0 (bytevector-length '#u8())))
(define qv '#(a (b lambda) #(define)))
($check (eq? 'a (ref qv 0)))
($check (equal? (list 'b 'lambda) (ref qv 1)))
($check (eq? 'define (ref (ref qv 2) 0)))

;; a quoted atom ends the quoted datum; the next form parses normally
'5
(define after-number (lambda (x) (+ x 1)))
($check (= 2 (after-number 1)))
'"str"
(define after-string (if #t 10 20))
($check (= 10 after-string))
'#t
(define after-bool (lambda (x) x))
($check (eq? 'ok (after-bool 'ok)))
'#\a
(define after-char 42)
($check (= 42 after-char))
