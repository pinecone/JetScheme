;; Number operations

;; Basic arithmetic
($check (= 0 (+ 0 0)))
($check (= 0 (- 5 5)))
($check (= 1 (* 1 1)))
($check (= 2 (/ 10 5)))

;; Variadic arithmetic
($check (= 10 (+ 1 2 3 4)))
($check (= 24 (* 1 2 3 4)))

;; Unary minus
($check (= -5 (- 5)))
($check (= 5 (- -5)))

;; Comparisons
($check (<= 1 1))
($check (<= 1 2))
($check (not (<= 2 1)))
($check (>= 2 2))
($check (>= 3 2))
($check (not (>= 1 2)))

;; Floor, abs, modulo
($check (= 3 (floor 3.7)))
($check (= 5 (abs -5)))
($check (= 5 (abs 5)))
($check (= 1 (modulo 7 3)))
($check (= 0 (modulo 6 3)))
($check (= -1 (modulo -7 -3)))
($check (= 2 (modulo -7 3)))
($check (= -2 (modulo 7 -3)))
($check (= 1.5 (modulo 7.5 2.0)))
($check (= 0.5 (modulo -7.5 2.0)))
($check (< (abs (- 1.794 (modulo -6.206 2.0))) 1e-9))

;; Quotient, remainder
($check (= 2 (quotient 7 3)))
($check (= -2 (quotient -7 3)))
($check (= -2 (quotient 7 -3)))
($check (= 1 (remainder 7 3)))
($check (= -1 (remainder -7 3)))
($check (= 1 (remainder 7 -3)))
($check (= 3.0 (quotient 7.5 2.0)))
($check (= 1.5 (remainder 7.5 2.0)))

;; quotient + remainder reconstruct dividend (truncation division)
($check (= 7 (+ (* (quotient 7 3) 3) (remainder 7 3))))
($check (= -7 (+ (* (quotient -7 3) 3) (remainder -7 3))))
($check (= 7 (+ (* (quotient 7 -3) -3) (remainder 7 -3))))

;; Exponentiation
($check (= 8 (expt 2 3)))
($check (= 1 (expt 5 0)))

;; Max, min
($check (= 5 (max 1 5 3)))
($check (= 1 (min 1 5 3)))

;; Predicates (integer? and zero? are defined in standard library)
($check (zero? 0))
($check (not (zero? 1)))

;; Hex literals, positive and negative
($check (= 255 0xff))
($check (= -255 -0xff))
($check (= 31 0x1f))
($check (= -31 -0x1f))

;; Signed decimal literals
($check (= 1 +1))
($check (= -1 -1))
($check (= 3.14 +3.14))

;; expt and sqrt edge cases
($check (= 0.5 (expt 2 -1)))
($check (= 0.25 (expt 2 -2)))
($check (= 1.4142135623730951 (expt 2 0.5)))
($check (= 1 (expt 0 0)))
($check (= 0 (expt 0 3)))
($check (= -8 (expt -2 3)))
($check (= 4 (expt -2 2)))
($check (= 2 (sqrt 4)))
($check (= 0 (sqrt 0)))
($check (= 1.5 (sqrt 2.25)))
($check (= 2.0000000000000004 (expt (sqrt 2) 2)))

;; division by zero gives an infinity, which is a number and compares
($check (> (/ 1 0) 0))
($check (< (- 0 (/ 1 0)) 0))
($check (number? (/ 1 0)))
($check (< 1 (/ 1 0)))
($check (> 1 (- 0 (/ 1 0))))
($check (= (/ 1 0) (/ 2 0)))
($check (not (= (/ 1 0) (- 0 (/ 1 0)))))
($check (= (/ 1 0) (+ 1 (/ 1 0))))

;; integer? and exact? answer for every integral value
($check (integer? 2))
($check (integer? 2.0))
($check (integer? -7))
($check (not (integer? 2.5)))
($check (exact? 2))
($check (not (exact? 2.5)))
