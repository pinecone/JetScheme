; basic produce, final body value, completion, exhaustion via alternate
(define c1 (let/coro yield () (yield 1) (yield 2) 3))
($check (coroutine? c1))
($check (= 1 (%iter-next! c1 (v) v)))
($check (= 2 (%iter-next! c1 (v) v)))
($check (= 3 (%iter-next! c1 (v) v)))
($check (eq? 'done (%iter-next! c1 (v) v 'done)))

; body ending in a tail yield produces the value, then unknown (the yield expression's value)
(define c2 (let/coro yield () (yield 2)))
($check (= 2 (%iter-next! c2 (v) v)))
($check (eq? #t (%iter-next! c2 (v) #t)))
($check (eq? 'done (%iter-next! c2 (v) v 'done)))

; %iter returns the coroutine unchanged
(define c3 (let/coro yield () (yield 'a)))
($check (eq? c3 (%iter c3)))

; bindings carry initial state; body runs on first resume
(define c4 (let/coro yield ((a 10) (b 20)) (yield (+ a b)) (* a b)))
($check (= 30 (%iter-next! c4 (v) v)))
($check (= 200 (%iter-next! c4 (v) v)))

; yield is first-class: passed to a helper
(define c5 (let/coro yield () (for-each yield '(1 2 3)) 'end))
($check (= 1 (%iter-next! c5 (v) v)))
($check (= 2 (%iter-next! c5 (v) v)))
($check (= 3 (%iter-next! c5 (v) v)))
($check (eq? 'end (%iter-next! c5 (v) v)))

; let/ec wholly inside the body, extent spanning a yield
(define c6
  (let/coro yield ()
    (let/ec escape ()
      (yield 'first)
      (escape 'second)
      'unreached)))
($check (eq? 'first (%iter-next! c6 (v) v)))
($check (eq? 'second (%iter-next! c6 (v) v)))

; crossing escape: body calls an escape whose let/ec is on the main stack
(define c7 #f)
(define r7
  (let/ec escape ()
    (set! c7 (let/coro yield () (escape 'jumped) (yield 'unreached)))
    (%iter-next! c7 (v) v)
    'unreached))
($check (eq? 'jumped r7))

; flat-closure captures
(define (gen-from base)
  (let/coro yield () (yield base) (yield (+ base 1)) 'g))
(define g1 (gen-from 100))
(define g2 (gen-from 500))
($check (= 100 (%iter-next! g1 (v) v)))
($check (= 500 (%iter-next! g2 (v) v)))
($check (= 101 (%iter-next! g1 (v) v)))
($check (= 501 (%iter-next! g2 (v) v)))

; deep non-tail recursion inside the coroutine: stack growth by relocation
(define c8
  (let/coro yield ()
    (define (sum n) (if (= n 0) 0 (+ n (sum (- n 1)))))
    (yield (sum 20000))
    'deep-done))
($check (= 200010000 (%iter-next! c8 (v) v)))
($check (eq? 'deep-done (%iter-next! c8 (v) v)))

; nested coroutines: outer pumps inner
(define inner (let/coro yield () (yield 1) (yield 2) 'fin))
(define outer
  (let/coro yield ()
    (let loop ()
      (%iter-next! inner (v) (begin (yield v) (loop)) 'outer-end))))
($check (= 1 (%iter-next! outer (v) v)))
($check (= 2 (%iter-next! outer (v) v)))
($check (eq? 'fin (%iter-next! outer (v) v)))
($check (eq? 'outer-end (%iter-next! outer (v) v)))

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
(for-each (lambda (c) ($check (= 0 (%iter-next! c (v) v)))) cs)
(for-each (lambda (c) ($check (= 1 (%iter-next! c (v) v)))) cs)
(for-each (lambda (c) ($check (= 2 (%iter-next! c (v) v)))) cs)
(displayn "coroutines ok")
