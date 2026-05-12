;; set! on a primitive: subsequent calls see the new value; restoring brings
;; back the original.
;;
;; Note: + - * / and the comparison ops are compiled directly as arithmetic
;; and ignore set!, so we use modulo here, which goes through the symbol's
;; binding.

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
