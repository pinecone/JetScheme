;; List operations

;; cons, basic lists
($check (equal? '(1 2 3 4 5) '(1 2 3 4 5)))
($check (equal? '(1 2 3) (cons 1 (cons 2 (cons 3 '())))))
($check (equal? '((1 2) 3 4 (5 6 (7 8 9 10)) ()) '((1 2) 3 4 (5 6 (7 8 9 10)) ())))
($check (null? '()))

;; car
($check (= 1 (car '(1 2 3 4))))
($check (equal? '(1 2 3) (car '((1 2 3) 4 5))))
($check (equal? '((1 2) 3) (car '(((1 2) 3)))))

;; cdr
($check (equal? '(2 3 4) (cdr '(1 2 3 4))))
($check (equal? '(4 5) (cdr '((1 2 3) 4 5))))
($check (null? (cdr '(1))))
($check (equal? '((2 () 3) 4) (cdr '(1 (2 () 3) 4))))

;; first, second
($check (eq? 'a (first '(a b c))))
($check (equal? '(1) (first '((1) 2))))
($check (eq? 'b (second '(a b c))))
($check (equal? '((2)) (second '((1) ((2))))))

;; caar, cadr, cdar, cddr, caddr, cdddr
($check (eq? 'c (caar '((c a r) (j e t)))))
($check (equal? '(c a r) (caar '(((c a r))))))
($check (eq? 'b (cadr '(a b c))))
($check (equal? '(b c) (cadr '((a)(b c)(d e f)))))
($check (equal? '(b) (cdar '((a b) c))))
($check (equal? '((b c) d) (cdar '((a (b c) d)))))
($check (equal? '(c) (cddr '(a b c))))
($check (equal? '((c d) e) (cddr '(a b (c d) e))))
($check (eq? 'c (caddr '(a b c))))
($check (equal? '(c d) (caddr '(a b (c d) e))))
($check (equal? '(d) (cdddr '(a b c d))))
($check (equal? '((d e) f g) (cdddr '(a b c (d e) f g))))

;; list
($check (equal? '(a b) (list 'a 'b)))
($check (equal? '(1 2 3 4 5 6 7) (list 1 2 3 4 5 6 7)))
($check (equal? '(1 2 (3 4) a) (list 1 2 '(3 4) 'a)))

;; length
($check (= 0 (length '())))
($check (= 1 (length '(1))))
($check (= 3 (length '(1 2 3))))
($check (= 4 (length '(1 2 (3 (4)) 5))))

;; reverse
($check (equal? '(5 4 3 2 1) (reverse '(1 2 3 4 5))))
($check (null? (reverse '())))

;; append
($check (equal? '(1 2 3 4 5 6 7) (append '(1 2 3) '() '(4 5) '() '(6 7) '())))
($check (null? (append '() '() '())))

;; take, drop
($check (equal? '(a b) (take '(a b c d e) 2)))
($check (equal? '(c d e) (drop '(a b c d e) 2)))

(define foo (cons 1 (cons 2 (cons 3 '()))))
($check (equal? '(1 2) (take foo 2)))
($check (equal? '(3) (drop foo 2)))
($check (equal? '(1 2 3) (take foo 3)))
($check (null? (drop foo 3)))

;; last
($check (= 4 (last '(1 2 3 4))))
($check (= 1 (last '(1))))

;; concatenate
($check (equal? '(1 2 3 4 5 6 7 8 9) (concatenate '((1 2 3) () (4 5 6) () (7 8 9)))))
($check (null? (concatenate '())))

;; Association lists

(define a '((a . 1) (b . 2) (c . 3)))
($check (equal? '(a . 1) (assq 'a a)))
($check (equal? '(c . 3) (assq 'c a)))
($check (eqv? #f (assq 'd a)))
($check (eqv? #f (assq 'a '())))

(define n '((1 . one) (2 . two) (3 . three)))
($check (equal? '(2 . two) (assv 2 n)))
($check (eqv? #f (assv 4 n)))

(define s '(("k1" . 10) ("k2" . 20) ("k3" . 30)))
($check (equal? '("k2" . 20) (assoc "k2" s)))
($check (eqv? #f (assoc "k4" s)))

;; equal? compares structure, so assoc finds composite keys
(define c '(((1 2) . a) ((3 4) . b)))
($check (equal? '((3 4) . b) (assoc '(3 4) c)))

;; duplicate keys: returns the first match
(define d '(("x" . 1) ("x" . 2) ("y" . 3)))
($check (equal? '("x" . 1) (assoc "x" d)))

;; mutating the returned pair updates the alist (assoc returns the cell)
(define e (list (cons "k" 1) (cons "j" 2)))
(set-cdr! (assoc "k" e) 99)
($check (equal? '(("k" . 99) ("j" . 2)) e))

;; values can be anything
(define f (list (cons "list" '(1 2 3)) (cons "vec" #(4 5)) (cons "nested" '(("x" . 10)))))
($check (equal? '(1 2 3) (cdr (assoc "list" f))))
($check (equal? #(4 5) (cdr (assoc "vec" f))))
($check (= 10 (cdr (assoc "x" (cdr (assoc "nested" f))))))

;; reader supports dotted pairs
($check (equal? (cons 1 2) '(1 . 2)))
($check (equal? (cons 1 (cons 2 3)) '(1 2 . 3)))

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
