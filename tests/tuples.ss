(define empty-a (tuple))
(define empty-b (tuple))

($check (isa? empty-a tuple))
($check (not (eq? empty-a empty-b)))
($check (equal? empty-a empty-b))

(define values (tuple 10 'two "three" '(4 5)))
($check (isa? values tuple))
($check (= 10 (ref values 0)))
($check (eq? 'two (ref values 1)))
($check (equal? "three" (ref values 2)))
($check (equal? '(4 5) (ref values 3)))

(define index 2)
($check (equal? "three" (ref values index)))

(define probe-type (struct 'probe '(value)))
(define probe (probe-type 'ordinary))
(define (cached-ref object key) (ref object key))
($check (eq? 'ordinary (cached-ref probe 'value)))
($check (= 10 (cached-ref values 0)))
($check (eq? 'ordinary (cached-ref probe 'value)))

(define same-values (tuple 10 'two "three" '(4 5)))
(define different-values (tuple 10 'two "three" '(4 6)))
(define applied (apply tuple '(10 two "three" (4 5))))
($check (not (eq? values same-values)))
($check (equal? values same-values))
($check (equal? values applied))
($check (not (equal? values different-values)))
($check (not (equal? values #(10 two "three" (4 5)))))

(define nested-a (tuple (tuple 1 2) (vector (tuple 3 4))))
(define nested-b (tuple (tuple 1 2) (vector (tuple 3 4))))
($check (equal? nested-a nested-b))

(define cycle-a-vector (vector #f))
(define cycle-a (tuple cycle-a-vector))
(vector-set! cycle-a-vector 0 cycle-a)
(define cycle-b-vector (vector #f))
(define cycle-b (tuple cycle-b-vector))
(vector-set! cycle-b-vector 0 cycle-b)
($check (equal? cycle-a cycle-b))

(define retained (tuple "alive" (list 1 2 3) (tuple 'nested)))
(define (allocate n)
  (if (= n 0)
      #t
      (begin
        (tuple n n n n)
        (allocate (- n 1)))))
(allocate 2000)
($check (equal? "alive" (ref retained 0)))
($check (equal? '(1 2 3) (ref retained 1)))
($check (equal? (tuple 'nested) (ref retained 2)))

(display values)
(write values)
