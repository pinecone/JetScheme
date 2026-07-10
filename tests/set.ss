;; Mutation with set!

(define a 5)
($check (= 5 a))

(set! a 6)
($check (= 6 a))

(define b a)
($check (= 6 b))

(set! a 7)
($check (= 6 b))  ;; b is not affected
($check (= 7 a))

;; set! with lists
(set! a '(1 2 3))
(set! b a)
(set! a '(1 2 3 4))
($check (equal? '(1 2 3 4) a))
($check (equal? '(1 2 3) b))

;; set-car!
(set! a '(1 2 3))
(set-car! a 2)
($check (equal? '(2 2 3) a))

;; set! with a complex RHS: the value is computed via an ANF temp, then stored.
(define sc 1)
(set! sc (+ sc (* 2 (+ sc 2))))
($check (= 7 sc))
(set! sc (if (> sc 5) (begin (set! sc 100) (+ sc 1)) 'small))
($check (= 101 sc))

;; set! of a captured+mutated (boxed) local with a complex RHS.
(define (make-acc)
  (let ((total 0))
    (lambda (n) (set! total (+ total n)) total)))
(define acc (make-acc))
(acc 3)
($check (= 10 (acc 7)))

;; set! on a primitive: subsequent calls see the new value; restoring brings
;; back the original. This holds even for ops that are otherwise lowered to
;; dedicated opcodes (arithmetic, comparisons, ref): a set! on the top-level
;; binding disables that lowering so the call goes through the live value.

(define orig-modulo modulo)

($check (= 1 (modulo 7 3)))         ;; original behavior

(set! modulo -)                     ;; redefine modulo to subtract
($check (= 4 (modulo 7 3)))         ;; new behavior

(set! modulo orig-modulo)           ;; restore
($check (= 1 (modulo 7 3)))         ;; original behavior again

;; The same dance with a unary primitive, just to exercise nargs=1.
(define orig-zero? zero?)
($check (zero? 0))
($check (not (zero? 1)))

(set! zero? (lambda (x) (= x 42)))
($check (zero? 42))
($check (not (zero? 0)))

(set! zero? orig-zero?)
($check (zero? 0))
($check (not (zero? 42)))

;; + lowers to add2ss (two operands) and add2sc (literal-constant operand);
;; set! must defeat both forms.
(define orig+ +)
($check (= 7 (+ 3 4)))              ;; add2ss
($check (= 6 (+ 5 1)))              ;; add2sc (literal rhs)

(set! + (lambda (a b) (* a b)))
($check (= 12 (+ 3 4)))             ;; add2ss now multiplies
($check (= 5 (+ 5 1)))              ;; add2sc now multiplies

(set! + orig+)
($check (= 7 (+ 3 4)))
($check (= 6 (+ 5 1)))

;; comparison ops lower too; redefine < and verify with the untouched =.
(define orig< <)
($check (< 1 2))
($check (not (< 2 1)))

(set! < >)                         ;; flip the comparison
($check (< 2 1))
($check (not (< 1 2)))

(set! < orig<)
($check (< 1 2))
($check (not (< 2 1)))

;; ref lowers to an element/field accessor; set! must defeat that too.
(define orig-ref ref)
($check (= 20 (ref (vector 10 20 30) 1)))

(set! ref (lambda (o k) 'redefined))
($check (equal? 'redefined (ref (vector 10 20 30) 1)))

(set! ref orig-ref)
($check (= 20 (ref (vector 10 20 30) 1)))
