;; set! on a primitive: subsequent calls see the new value; restoring brings
;; back the original.
;;
;; Note: + and - are compiled directly as arithmetic and ignore set!, so
;; we use * here, which goes through the symbol's binding.

(define orig* *)

($check (= 6 (* 2 3)))      ;; original behavior

(set! * -)                  ;; redefine * to subtract
($check (= -1 (* 2 3)))     ;; new behavior

(set! * orig*)              ;; restore
($check (= 6 (* 2 3)))      ;; original behavior again

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
