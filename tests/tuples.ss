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

;; #tuple(...) reader syntax: elements are data, templates unquote and splice.
($check (equal? (tuple 1 'two "three") #tuple(1 two "three")))
($check (isa? #tuple() tuple))
($check (equal? (tuple '(1 2) #(3)) #tuple((1 2) #(3))))
(define spliced (list 2 3))
($check (equal? (tuple 1 2 3) `#tuple(1 ,(first spliced) ,(second spliced))))
($check (equal? (tuple 1 2 3) `#tuple(1 ,@spliced)))

;; Tuples wider than the arena's size classes are allocated off-heap and must
;; survive collections that recycle their neighbours.
;; TODO: the width below is based on current jet allocator behaviour.
;; jet should expose the allocator configuration at runtime so this
;; test can derive actual limits and stay meaningful.
(define (span n acc) (if (= n 0) acc (span (- n 1) (cons n acc))))
(define wide (apply tuple (span 2000 '())))
(define (churn n) (when (> n 0) (apply tuple (span 2000 '())) (churn (- n 1))))
(churn 200)
($check (= (ref wide 0) 1))
($check (= (ref wide 1999) 2000))
($check (equal? wide (apply tuple (span 2000 '()))))
