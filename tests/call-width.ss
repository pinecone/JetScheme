;; Fixed-arity calls at 15, 16, and 17 arguments cover both sides of the fast-entry split.

(define (sum15 a b c d e f g h i j k l m n o)
  (+ a b c d e f g h i j k l m n o))
(define (sum16 a b c d e f g h i j k l m n o p)
  (+ a b c d e f g h i j k l m n o p))
(define (sum17 a b c d e f g h i j k l m n o p q)
  (+ a b c d e f g h i j k l m n o p q))

($check (= 120 (sum15 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15)))
($check (= 136 (sum16 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16)))
($check (= 153 (sum17 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17)))

;; The same widths in tail position
(define (tail15 x) (sum15 x x x x x x x x x x x x x x x))
(define (tail16 x) (sum16 x x x x x x x x x x x x x x x x))
(define (tail17 x) (sum17 x x x x x x x x x x x x x x x x x))

($check (= 15 (tail15 1)))
($check (= 32 (tail16 2)))
($check (= 51 (tail17 3)))

;; Tail self-call at 17 arguments loops through the slow path
(define (count17 a b c d e f g h i j k l m n o p q)
  (if (= a 0)
      q
      (count17 (- a 1) b c d e f g h i j k l m n o p (+ q 1))))
($check (= 1000 (count17 1000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)))

;; Variadic entry with rest-list length 0, 1, and many
(define rest-len (lambda args (length (cdr args))))
($check (= 0 (rest-len 'x)))
($check (= 1 (rest-len 'x 1)))
($check (= 5 (rest-len 'x 1 2 3 4 5)))

(define all-args (lambda args args))
($check (equal? '() (all-args)))
($check (equal? '(1) (all-args 1)))
($check (= 20 (length (all-args 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20))))

;; Variadic tail self-call
(define drain
  (lambda xs
    (if (null? xs)
        'done
        (apply drain (cdr xs)))))
($check (eq? 'done (drain 1 2 3 4 5)))
