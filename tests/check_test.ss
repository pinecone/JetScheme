;; Test the $check assertion form

($check (= 5 (+ 2 3)))
($check (< 1 2))
($check (eqv? 'a 'a))
($check (null? '()))
($check (= 42 (* 6 7)))

(displayn "all checks passed")
