;; Stackification correctness

;; A single-use binding captured by a closure must not contract: the val
;; runs once, not once per closure call.
(define calls 0)
(define (make-thunk)
  (let ((t (begin (set! calls (+ calls 1)) calls)))
    (lambda () t)))
(define th (make-thunk))
($check (= 1 (th)))
($check (= 1 (th)))
($check (= 1 calls))

;; A user-visible binding's val evaluates before the body reads anything,
;; even a same-combination atom.
(define x2 0)
(define (touch!) (set! x2 99) 7)
(define r-seq (let ((t (touch!))) (+ x2 t)))
($check (= 106 r-seq))

;; Zero-use begin-spine values still run for effect.
(define n2 0)
(define r-begin (begin (set! n2 5) (set! n2 (+ n2 1)) n2))
($check (= 6 r-begin))
($check (= 6 n2))

;; Contracted operand chains keep source evaluation order.
(define log2 (list))
(define (rec! v) (set! log2 (cons v log2)) v)
($check (= 6 (+ (rec! 1) (+ (rec! 2) (rec! 3)))))
($check (equal? (list 3 2 1) log2))

;; set! with a complex right-hand side through a contracted temp.
(define acc 1)
(set! acc (+ acc (+ acc acc)))
($check (= 3 acc))
