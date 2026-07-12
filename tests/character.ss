;; Characters

;; predicate
($check (char? #\a))
($check (not (char? "a")))
($check (not (char? 65)))

;; conversion
($check (= 65 (char->integer #\A)))
($check (= 48 (char->integer #\0)))
($check (eqv? #\A (integer->char 65)))
($check (eqv? #\0 (integer->char 48)))

;; comparison
($check (char=? #\a #\a))
($check (not (char=? #\a #\b)))
($check (char=? #\a #\a #\a))
($check (char<? #\a #\b))
($check (char<=? #\a #\a))
($check (char>? #\b #\a))
($check (char>=? #\a #\a))
($check (char-ci=? #\A #\a))
($check (char-ci<? #\a #\B))

;; classification
($check (char-alphabetic? #\a))
($check (char-alphabetic? #\Z))
($check (not (char-alphabetic? #\0)))
($check (not (char-alphabetic? #\space)))
($check (char-numeric? #\7))
($check (not (char-numeric? #\a)))
($check (char-whitespace? #\space))
($check (char-whitespace? #\tab))
($check (char-whitespace? #\newline))
($check (not (char-whitespace? #\a)))
($check (char-upper-case? #\A))
($check (not (char-upper-case? #\a)))
($check (char-lower-case? #\a))
($check (not (char-lower-case? #\A)))

;; case
($check (eqv? #\A (char-upcase #\a)))
($check (eqv? #\A (char-upcase #\A)))
($check (eqv? #\a (char-downcase #\A)))
($check (eqv? #\1 (char-upcase #\1)))

;; digit-value
($check (= 0 (digit-value #\0)))
($check (= 9 (digit-value #\9)))
($check (= -1 (digit-value #\a)))
