(define cursor (%iter (vector 10 20 30)))
($check (= 10 (%iter-next! cursor (value) value)))
($check (= 21 (%iter-next! cursor (value) (+ value 1))))
($check (= 30 (%iter-next! cursor (value) value 'exhausted)))
($check (eq? 'exhausted (%iter-next! cursor (value) value 'exhausted)))
(%iter-next! (%iter (vector)) (value) value)

($check (= 33 (%iter-next! (%iter (vector 33)) (value) value)))

(define (cursor-first cursor)
  (%iter-next! cursor (value) value 'empty))
($check (= 34 (cursor-first (%iter (vector 34)))))
($check (eq? 'empty (cursor-first (%iter (vector)))))

(define equal-cursor-target (vector 1 2))
(define equal-cursor-a (%iter equal-cursor-target))
(define equal-cursor-b (%iter equal-cursor-target))
(define equal-cursor-other (%iter (vector 1 2)))
($check (equal? equal-cursor-a equal-cursor-b))
($check (not (equal? equal-cursor-a equal-cursor-other)))
(%iter-next! equal-cursor-a (value) value)
($check (not (equal? equal-cursor-a equal-cursor-b)))
(%iter-next! equal-cursor-b (value) value)
($check (equal? equal-cursor-a equal-cursor-b))

(define outer 'outer)
(define scope-cursor (%iter (vector 'inner)))
($check (eq? 'inner (%iter-next! scope-cursor (outer) outer)))
($check (eq? 'outer outer))

(define capture-cursor (%iter (vector 40 50)))
(define captured (%iter-next! capture-cursor (value) (lambda () value)))
($check (= 40 (captured)))
(define mutated
  (%iter-next! capture-cursor (value)
    (begin
      (set! value (+ value 1))
      (lambda () value))))
($check (= 51 (mutated)))

(define (sum-cursor cursor total)
  (%iter-next! cursor (value)
    (sum-cursor cursor (+ total value))
    total))
($check (= 15 (sum-cursor (%iter (vector 1 2 3 4 5)) 0)))

(define outer-cursor (%iter (vector 1 2)))
(define inner-cursor (%iter (vector 10 20)))
($check
  (= 11
     (%iter-next! outer-cursor (outer-value)
       (%iter-next! inner-cursor (inner-value)
         (+ outer-value inner-value)))))

(define retained-cursor (%iter (vector 42)))
(let loop ((i 0))
  (if (< i 10000)
      (begin
        (vector i i i i i i i i)
        (loop (+ i 1)))))
($check (= 42 (%iter-next! retained-cursor (value) value)))
