;; let/ec -- escape continuations

;; No escape: the body's value.
($check (= 42 (let/ec k () 42)))
($check (eq? 'done (let/ec k () 'a 'b 'done)))

;; Escaping delivers the value.
($check (= 7 (let/ec k () (k 7) 'unreachable)))
($check (eq? #f (let/ec k () (k #f) 'unreachable)))
($check (equal? '(1 2) (let/ec k () (k (list 1 2)))))

;; Bindings behave like let, and are in scope for the body.
($check (= 3 (let/ec k ((a 1) (b 2)) (+ a b))))
($check (= 1 (let/ec k ((a 1) (b 2)) (k a) b)))

;; k is in scope for the initializers, so an initializer can escape.
($check (eq? 'from-init (let/ec k ((a (k 'from-init))) 'unreachable)))

;; Escaping out of a nested lambda, through a higher-order prelude call.
(define (first-even xs)
  (let/ec k ()
    (for-each (lambda (x) (when (even? x) (k x))) xs)
    #f))
($check (= 8 (first-even '(1 3 8 9))))
($check (= 2 (first-even '(2 4 6))))
($check (eq? #f (first-even '(1 3 5))))

;; Escaping out of map, whose frames are discarded mid-traversal.
(define (has-negative? xs)
  (let/ec k ()
    (map (lambda (x) (if (negative? x) (k #t) x)) xs)
    #f))
($check (eq? #t (has-negative? '(1 -2 3))))
($check (eq? #f (has-negative? '(1 2 3))))

;; Escaping from deep non-tail recursion: 20000 pending frames are dropped.
(define (deep n k)
  (if (zero? n)
      (k 'bottom)
      (+ 1 (deep (- n 1) k))))
($check (eq? 'bottom (let/ec k () (deep 20000 k))))

;; Escaping across self-tail-call (recur) frames, which reuse one frame.
(define (spin n k)
  (if (zero? n) (k 'spun) (spin (- n 1) k)))
($check (eq? 'spun (let/ec k () (spin 100000 k))))

;; Tail calls inside the body stay tail calls: this loop would overflow if the
;; body's frame were retained per iteration.
(define (count n)
  (if (zero? n) 'counted (count (- n 1))))
($check (eq? 'counted (let/ec k () (count 1000000))))

;; A captured, mutated binding survives an escape past its frame.
(define (sum-until-negative xs)
  (let/ec k ((total 0))
    (for-each (lambda (x)
                (when (negative? x) (k (list 'stopped-at total)))
                (set! total (+ total x)))
              xs)
    total))
($check (= 6 (sum-until-negative '(1 2 3))))
($check (equal? '(stopped-at 3) (sum-until-negative '(1 2 -9 4))))

;; Nested let/ec: the inner escape returns to the inner form.
($check (eq? 'inner
             (let/ec outer ()
               (let/ec inner () (inner 'inner)))))

;; The outer escape leaves both forms at once.
($check (eq? 'both
             (let/ec outer ()
               (let/ec inner () (outer 'both))
               'unreachable)))

;; Escapes nest three deep and each one reaches its own form.
($check (equal? '(a b c)
                (list (let/ec k1 () (let/ec k2 () (let/ec k3 () (k1 'a))))
                      (let/ec k1 () (let/ec k2 () (let/ec k3 () (k2 'b)) 'unreachable))
                      (let/ec k1 () (let/ec k2 () (let/ec k3 () (k3 'c)))))))

;; An escape handed to another procedure still returns to its own form.
(define (call-it f v) (f v))
($check (eq? 'indirect (let/ec k () (call-it k 'indirect) 'unreachable)))

;; let/ec in tail position of a procedure.
(define (tail-escape x)
  (let/ec k () (when x (k 'escaped)) 'fell-through))
($check (eq? 'escaped (tail-escape #t)))
($check (eq? 'fell-through (tail-escape #f)))

;; Repeated entry: a fresh escape per iteration, with allocation in between to
;; force collections during the extents.
(define (make-garbage n)
  (if (zero? n) '() (cons (vector n n) (make-garbage (- n 1)))))
(define (probe i)
  (let/ec k ()
    (make-garbage 50)
    (when (even? i) (k 'even))
    (make-garbage 50)
    'odd))
(define (run-probes i evens)
  (if (= i 2000)
      evens
      (run-probes (+ i 1) (if (eq? 'even (probe i)) (+ evens 1) evens))))
($check (= 1000 (run-probes 0 0)))

;; The escape is a value: identity holds, and it is not a procedure.
($check (let/ec k () (eq? k k)))
($check (let/ec k () (equal? k k)))
($check (eq? #f (let/ec k () (procedure? k))))
($check (let/ec k1 () (let/ec k2 () (eq? #f (eq? k1 k2)))))
