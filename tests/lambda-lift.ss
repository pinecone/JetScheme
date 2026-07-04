;; Lambda lift: letrec/named-let lambdas whose captures become parameters

;; basic shape: one immutable capture
(define (run limit offset)
  (letrec ((loop (lambda (n i)
                   (if (< i n) (loop n (+ i offset)) i))))
    (loop limit 0)))
($check (= 10 (run 10 5)))

;; two captures, deep self-recursion
(define (gauss lim a b)
  (letrec ((go (lambda (i acc)
                 (if (< i lim) (go (+ i 1) (+ acc (+ a b))) acc))))
    (go 0 0)))
($check (= 300000 (gauss 100000 1 2)))

;; nested named lets: the inner lift's rewritten call sites feed the outer lift
(define (fill out xs)
  (let loop ((todo xs) (acc '()))
    (if (null? todo)
        acc
        (let ((v (car todo)))
          (let ((next (let walk ((c v) (a acc))
                        (if (< c out) (walk (+ c 1) (cons c a)) a))))
            (loop (cdr todo) next))))))
($check (equal? '(2 2 1) (fill 3 '(1 2))))

;; set! of a capture inside the loops: writes must reach the shared binding
(define (count-pairs groups)
  (let ((count 0))
    (for-each
      (lambda (reduced)
        (let outer ((xs reduced))
          (when (pair? xs)
            (let ((m1 (car xs)))
              (let inner ((ys (cdr xs)))
                (when (pair? ys)
                  (when (eq? m1 (car ys))
                    (set! count (+ count 1)))
                  (inner (cdr ys))))
              (outer (cdr xs))))))
      groups)
    count))
($check (= 4 (count-pairs (list (list 1 2 1 1) (list 3 3)))))

;; the lambda escapes as a value from its own body: arity must not change
(define (make-counter step)
  (letrec ((tick (lambda (n)
                   (if (< n 1) tick (tick (- n step))))))
    (tick 10)))
($check (procedure? (((make-counter 2) 0) 5)))

;; the lambda escapes as a value from the let body
(define (pick off)
  (letrec ((g (lambda (x) (+ x off))))
    (map g '(1 2 3))))
($check (equal? '(11 12 13) (pick 10)))

;; non-tail self call
(define (sum-to lim add-on)
  (letrec ((go (lambda (i)
                 (if (< i lim) (+ add-on (go (+ i 1))) 0))))
    (go 0)))
($check (= 50 (sum-to 5 10)))

;; binding reassigned after init
(define (twice off)
  (letrec ((f (lambda (x) (+ x off))))
    (set! f (lambda (x) (* x off)))
    (f 3)))
($check (= 12 (twice 4)))

;; nested lambda inside the loop capturing the loop's own state
(define (tab off n)
  (letrec ((build (lambda (i acc)
                    (if (< i n)
                        (build (+ i 1) (cons (lambda () (+ i off)) acc))
                        acc))))
    (map (lambda (f) (f)) (build 0 '()))))
($check (equal? '(102 101 100) (tab 100 3)))
