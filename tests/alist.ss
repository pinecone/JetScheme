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
