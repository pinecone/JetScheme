; basic produce, final body value, completion, exhaustion via alternate
(define c1 (let/coro yield () (yield 1) (yield 2) 3))
($check (coroutine? c1))
($check (= 1 (%if/next! ((v) c1) v)))
($check (= 2 (%if/next! ((v) c1) v)))
($check (= 3 (%if/next! ((v) c1) v)))
($check (eq? 'done (%if/next! ((v) c1) v 'done)))

; body ending in a tail yield produces the value, then unknown (the yield expression's value)
(define c2 (let/coro yield () (yield 2)))
($check (= 2 (%if/next! ((v) c2) v)))
($check (eq? #t (%if/next! ((v) c2) #t)))
($check (eq? 'done (%if/next! ((v) c2) v 'done)))

; %iter returns the coroutine unchanged
(define c3 (let/coro yield () (yield 'a)))
($check (eq? c3 (%iter c3)))

; bindings carry initial state; body runs on first resume
(define c4 (let/coro yield ((a 10) (b 20)) (yield (+ a b)) (* a b)))
($check (= 30 (%if/next! ((v) c4) v)))
($check (= 200 (%if/next! ((v) c4) v)))

; yield is first-class: passed to a helper
(define c5 (let/coro yield () (for-each yield '(1 2 3)) 'end))
($check (= 1 (%if/next! ((v) c5) v)))
($check (= 2 (%if/next! ((v) c5) v)))
($check (= 3 (%if/next! ((v) c5) v)))
($check (eq? 'end (%if/next! ((v) c5) v)))

; let/ec wholly inside the body, extent spanning a yield
(define c6
  (let/coro yield ()
    (let/ec escape ()
      (yield 'first)
      (escape 'second)
      'unreached)))
($check (eq? 'first (%if/next! ((v) c6) v)))
($check (eq? 'second (%if/next! ((v) c6) v)))

; crossing escape: body calls an escape whose let/ec is on the main stack
(define c7 #f)
(define r7
  (let/ec escape ()
    (set! c7 (let/coro yield () (escape 'jumped) (yield 'unreached)))
    (%if/next! ((v) c7) v)
    'unreached))
($check (eq? 'jumped r7))

; flat-closure captures
(define (gen-from base)
  (let/coro yield () (yield base) (yield (+ base 1)) 'g))
(define g1 (gen-from 100))
(define g2 (gen-from 500))
($check (= 100 (%if/next! ((v) g1) v)))
($check (= 500 (%if/next! ((v) g2) v)))
($check (= 101 (%if/next! ((v) g1) v)))
($check (= 501 (%if/next! ((v) g2) v)))

; deep non-tail recursion inside the coroutine: stack growth by relocation
(define c8
  (let/coro yield ()
    (define (sum n) (if (= n 0) 0 (+ n (sum (- n 1)))))
    (yield (sum 20000))
    'deep-done))
($check (= 200010000 (%if/next! ((v) c8) v)))
($check (eq? 'deep-done (%if/next! ((v) c8) v)))

; nested coroutines: outer pumps inner
(define inner (let/coro yield () (yield 1) (yield 2) 'fin))
(define outer
  (let/coro yield ()
    (let loop ()
      (%if/next! ((v) inner) (begin (yield v) (loop)) 'outer-end))))
($check (= 1 (%if/next! ((v) outer) v)))
($check (= 2 (%if/next! ((v) outer) v)))
($check (eq? 'fin (%if/next! ((v) outer) v)))
($check (eq? 'outer-end (%if/next! ((v) outer) v)))

; identity equality
(define c9 (let/coro yield () 1))
($check (eq? c9 c9))
($check (eqv? c9 c9))
($check (equal? c9 c9))
($check (eq? #f (eq? c9 c1)))

; many live coroutines across GC
(define (make-counter n)
  (let/coro yield ()
    (let loop ((i 0))
      (when (< i n) (yield i) (loop (+ i 1))))
    'counted))
(define cs '())
(let loop ((k 0))
  (when (< k 200)
    (set! cs (cons (make-counter 5) cs))
    (loop (+ k 1))))
(for-each (lambda (c) ($check (= 0 (%if/next! ((v) c) v)))) cs)
(for-each (lambda (c) ($check (= 1 (%if/next! ((v) c) v)))) cs)
(for-each (lambda (c) ($check (= 2 (%if/next! ((v) c) v)))) cs)
(displayn "coroutines ok")
