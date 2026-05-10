;; Internal defines: letrec*-style hoist into the enclosing lambda body.

;; Single internal define.
(define (f1) (define a 1) a)
($check (= 1 (f1)))

;; Multiple internal defines.
(define (f2) (define a 1) (define b 2) (+ a b))
($check (= 3 (f2)))

;; Forward reference: g's body refs h, defined later in the same body.
(define (f3)
  (define g (lambda () h))
  (define h 42)
  (g))
($check (= 42 (f3)))

;; Mutual recursion across internal defines.
(define (parity n)
  (define (even? n) (if (= n 0) #t (odd? (- n 1))))
  (define (odd?  n) (if (= n 0) #f (even? (- n 1))))
  (even? n))
($check (eq? #t (parity 10)))
($check (eq? #f (parity 11)))

;; set! on an internal-define binding.
(define (f5)
  (define a 1)
  (set! a 2)
  a)
($check (= 2 (f5)))

;; Lenient mode: defines may be interleaved with expressions; all hoist.
(define (f6)
  (define a 1)
  (set! a 2)
  (define b 3)
  (+ a b))
($check (= 5 (f6)))

;; Defines inside a begin block in body position.
(define (f7)
  (begin (define a 1) (define b 2))
  (+ a b))
($check (= 3 (f7)))

;; Defines inside a let body.
(define (f8)
  (let ()
    (define a 1)
    a))
($check (= 1 (f8)))

;; Internal define captured by an inner closure.
(define (f9)
  (define a 7)
  (lambda () a))
($check (= 7 ((f9))))

;; Nested lambdas: inner defines don't leak to outer.
(define (f10)
  (define x 1)
  (define (g)
    (define x 2)
    x)
  (+ x (g)))
($check (= 3 (f10)))

;; Top-level binding shadowed by an internal define.
(define gx 100)
(define (f11)
  (define gx 1)
  gx)
($check (= 1 (f11)))
($check (= 100 gx))

;; (define x x): reading the binding before its initializer returns #f.
(define (f12)
  (define x x)
  x)
($check (eq? #f (f12)))

;; (define (h args) body) shorthand inside another body.
(define (f13)
  (define (h n) (* n n))
  (h 5))
($check (= 25 (f13)))

;; Outer param visible to internal-define initializer.
(define (f14 outer)
  (define inner (* outer 2))
  inner)
($check (= 14 (f14 7)))

;; Mutually recursive internal defines that form a cycle through closures.
(define (count-down n)
  (define (loop k acc)
    (if (= k 0) acc (loop (- k 1) (+ acc 1))))
  (loop n 0))
($check (= 5 (count-down 5)))

;; Internal define whose RHS is a lambda capturing an outer param.
(define (adder x)
  (define f (lambda (y) (+ x y)))
  f)
($check (= 9 ((adder 4) 5)))

;; Define inside the body of a let that itself binds a name.
(define (f17)
  (let ((a 10))
    (define b (* a 2))
    (+ a b)))
($check (= 30 (f17)))

;; Top-level forward reference: f's body refs g, defined later.
(define f-fwd (lambda () g-fwd))
(define g-fwd 42)
($check (= 42 (f-fwd)))

;; Top-level mutual recursion across define-bound lambdas.
(define (tl-even? n) (if (= n 0) #t (tl-odd?  (- n 1))))
(define (tl-odd?  n) (if (= n 0) #f (tl-even? (- n 1))))
($check (eq? #t (tl-even? 8)))
($check (eq? #t (tl-odd? 9)))

;; Redefine at top level: later define wins.
(define rd 1)
($check (= 1 rd))
(define rd 2)
($check (= 2 rd))

;; Top-level define inside a (begin ...) sequence.
(begin
  (define bg-a 10)
  (define bg-b 20))
($check (= 30 (+ bg-a bg-b)))

;; Top-level define whose RHS reads an earlier top-level binding.
(define seq-a 5)
(define seq-b (* seq-a 3))
($check (= 15 seq-b))

;; Define inside a begin inside a lambda body shadows an outer top-level
;; binding. The inner define is local to the lambda, so the outer binding
;; must stay untouched.
(define shx 1)
(define (sh-call)
  (begin
    (define shx 2)
    (set! shx 3))
  shx)
($check (= 3 (sh-call)))
($check (= 1 shx))

;; Same shape inside a let body.
(define lhx 1)
(define lhx-result
  (let ()
    (define lhx 99)
    lhx))
($check (= 99 lhx-result))
($check (= 1 lhx))
