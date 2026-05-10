;; Arithmetic operations

($check (= -4 (- 4)))
($check (not (= -5 (- 4))))

($check (= 0 0))
($check (not (= 1 0)))

($check (< 0 1))
($check (not (> 0 1)))

($check (= (+ 2 3) 5))
($check (not (= (+ 2 3) 6)))

($check (= (* 7 8) 56))
($check (not (= (* 7 8) 21)))

($check (= (/ 9 3) 3))
($check (not (= (/ 9 3) 1)))

;; Compound expression
($check (= 0 (- (+ 2 (* 3 (/ 10 5))) 8)))

;; Hex literal
($check (= 113 0x71))
