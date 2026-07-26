;; Quasiquote: unquote, splicing, dotted tails, vectors, nesting

(define x 42)
(define y 'sym)
(define lst '(1 2 3))
(define empty '())

;; no unquote: identical to quote
($check (equal? '(a b c) `(a b c)))
($check (eq? 'a `a))
($check (= 7 `7))
($check (equal? "s" `"s"))
($check (null? `()))
($check (equal? '(a (b (c))) `(a (b (c)))))
($check (equal? '(a . b) `(a . b)))

;; unquote
($check (equal? '(1 42 3) `(1 ,x 3)))
($check (equal? '(42) `(,x)))
($check (= 42 `,x))
($check (equal? '(a sym) `(a ,y)))
($check (equal? '(1 3) `(1 ,(+ 1 2))))
($check (equal? '(1 (2 3)) `(1 ,(list 2 3))))

;; unquote evaluates a full expression, keywords included
($check (equal? '(1 yes) `(1 ,(if #t 'yes 'no))))
($check (equal? '(6) `(,(let ((a 1) (b 2) (c 3)) (+ a b c)))))
($check (equal? '(9) `(,((lambda (n) (* n n)) 3))))

;; unquote in nested template positions
($check (equal? '(a (b 42)) `(a (b ,x))))
($check (equal? '(((42))) `(((,x)))))

;; splicing
($check (equal? '(a 1 2 3 b) `(a ,@lst b)))
($check (equal? '(1 2 3 b) `(,@lst b)))
($check (equal? '(a 1 2 3) `(a ,@lst)))
($check (equal? '(1 2 3) `(,@lst)))
($check (equal? '(1 2 3 1 2 3) `(,@lst ,@lst)))
($check (equal? '(a b) `(a ,@empty b)))
($check (equal? '() `(,@empty)))
($check (equal? '(a 1 2 3 42) `(a ,@lst ,x)))
($check (equal? '(0 1 2 3) `(,@(list 0) ,@lst)))

;; dotted tails
($check (equal? '(a . 42) `(a . ,x)))
($check (equal? '(1 2 . 3) `(1 ,(+ 1 1) . ,(+ 1 2))))
($check (equal? (cons 1 lst) `(1 . ,lst)))
($check (equal? '(1 1 2 3 . 9) `(1 ,@lst . 9)))
($check (equal? '(a b . c) `(a b . c)))

;; vectors
($check (equal? #(1 42 3) `#(1 ,x 3)))
($check (equal? #(a b) `#(a b)))
($check (equal? #(1 1 2 3) `#(1 ,@lst)))
($check (equal? #(1 2 3 4) `#(,@lst 4)))
($check (equal? #() `#()))

;; quote inside a template
($check (equal? '(quote 42) `(quote ,x)))
($check (equal? '(a (quote 42)) `(a ',x)))
($check (equal? '(a (quote b)) `(a 'b)))

;; nesting: an inner quasiquote raises the level, so its unquotes stay data
($check (equal? '(a (quasiquote (b (unquote (+ 1 2)))))
                `(a `(b ,(+ 1 2)))))
($check (equal? '(a (quasiquote (b (unquote-splicing (list 1 2)))))
                `(a `(b ,@(list 1 2)))))

;; ,,e -- the inner unquote is at level 1 again, so it evaluates
($check (equal? '(a (quasiquote (b (unquote 3))))
                `(a `(b ,,(+ 1 2)))))
($check (equal? '(quasiquote (unquote 42)) `(quasiquote ,,x)))
($check (equal? '(a (quasiquote (b (unquote 1) (unquote 2))))
                `(a `(b ,,(+ 0 1) ,,(+ 0 2)))))

;; ,,@e -- the values land in the tail of the rebuilt unquote form
($check (equal? '(a (quasiquote (b (unquote 1 2 3))))
                `(a `(b ,,@(list 1 2 3)))))

;; three levels deep: two commas stay data, the innermost one evaluates
($check (equal? '(quasiquote (quasiquote (unquote (unquote 42))))
                `(quasiquote `,,,x)))

;; the result is fresh, mutable structure
(define built `(1 ,x))
(set-car! built 99)
($check (equal? '(99 42) built))
($check (equal? '(1 42) `(1 ,x)))

;; the (quasiquote t) / (unquote e) / (unquote-splicing e) spellings
($check (equal? '(1 42) (quasiquote (1 (unquote x)))))
($check (equal? '(1 1 2 3) (quasiquote (1 (unquote-splicing lst)))))
($check (equal? '(1 42) `(1 (unquote x))))
($check (equal? '(1 1 2 3) `(1 (unquote-splicing lst))))
($check (equal? '(quasiquote (unquote 42)) `(quasiquote ,,x)))
($check (equal? 42 (quasiquote (unquote x))))

;; quasiquote is an expression, so it composes
($check (equal? '((1 42) (1 42)) (list `(1 ,x) `(1 ,x))))
($check (equal? '(0 1 42) (cons 0 `(1 ,x))))
($check (equal? 2 (length `(,x ,y))))
