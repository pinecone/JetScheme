;; Variable binding
(define alice "I love Bob!")
(define carol alice)

($check (eqv? "I love Bob!" carol))
($check (eqv? "I love Bob!" alice))
