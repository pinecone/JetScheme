;; String operations

;; append
(define x "h")
(define y "amburger")
($check (equal? "hamburger hamburger" (string-append x y " " x y)))

;; eqv?/eq? on strings are pointer identity, not value.
($check (not (eqv? (string-append "a" "b") "ab")))
($check (not (eq? (string-append "a" "b") "ab")))
(define same-str "hello")
($check (eqv? same-str same-str))
($check (eq? same-str same-str))
($check (string=? "" (string-append)))

;; predicates
($check (string? "abc"))
($check (not (string? 'abc)))
($check (not (string? 42)))

;; length / ref
($check (= 5 (string-length "hello")))
($check (= 0 (string-length "")))
($check (eqv? #\h (string-ref "hello" 0)))
($check (eqv? #\o (string-ref "hello" 4)))

;; constructors
($check (string=? "" (make-string 0)))
($check (string=? "aaaa" (make-string 4 #\a)))
($check (string=? "abc" (string #\a #\b #\c)))
($check (string=? "" (string)))

;; mutation
(define m (make-string 3 #\x))
(string-set! m 1 #\y)
($check (string=? m "xyx"))

(define f (make-string 5 #\.))
(string-fill! f #\!)
($check (string=? f "!!!!!"))
(string-fill! f #\? 1 4)
($check (string=? f "!???!"))

;; copy
($check (string=? "ell" (substring "hello" 1 4)))
($check (string=? "hello" (substring "hello" 0)))
($check (string=? "lo" (substring "hello" 3)))
($check (string=? "hello" (string-copy "hello")))
($check (string=? "ell" (string-copy "hello" 1 4)))

(define dst (make-string 5 #\.))
(string-copy! dst 1 "abc")
($check (string=? dst ".abc."))
(string-copy! dst 0 "xyzzy" 2 4)
($check (string=? dst "zzbc."))

;; comparison
($check (string=? "abc" "abc"))
($check (not (string=? "abc" "abd")))
($check (string=? "x" "x" "x"))
($check (string<? "abc" "abd"))
($check (string<=? "abc" "abc"))
($check (string>? "abd" "abc"))
($check (string>=? "abc" "abc"))
($check (string-ci=? "AbC" "aBc"))
($check (string-ci<? "abc" "ABD"))

;; case
($check (string=? "HELLO" (string-upcase "Hello")))
($check (string=? "hello" (string-downcase "Hello")))
($check (string=? "" (string-upcase "")))

;; conversion
($check (equal? '(#\a #\b #\c) (string->list "abc")))
($check (equal? '(#\b #\c) (string->list "abc" 1)))
($check (equal? '(#\b) (string->list "abc" 1 2)))
($check (string=? "abc" (list->string '(#\a #\b #\c))))
($check (string=? "" (list->string '())))

($check (equal? #(#\a #\b #\c) (string->vector "abc")))
($check (string=? "abc" (vector->string #(#\a #\b #\c))))

;; numbers
($check (= 42 (string->number "42")))
($check (= -3.5 (string->number "-3.5")))
($check (= 1e3 (string->number "1e3")))
($check (eqv? #f (string->number "abc")))
($check (eqv? #f (string->number "")))
($check (= 255 (string->number "ff" 16)))
($check (= 8 (string->number "1000" 2)))

($check (string=? "42" (number->string 42)))
($check (string=? "-3.5" (number->string -3.5)))
($check (string=? "ff" (number->string 255 16)))
($check (string=? "1000" (number->string 8 2)))

;; iter (prelude-side)
(define seen 0)
(string-for-each (lambda (c) (set! seen (+ seen (char->integer c)))) "abc")
($check (= (+ 97 98 99) seen))

($check (string=? "ABC" (string-map char-upcase "abc")))

;; Display and string values
(define black_things '("soot" "licorice" "midnight" "some chalkboards"
                       "tabbed\tthing" "before newline\nafter newline"
                       "\"quoted thing\"" "slish\\slash"))

($check (eqv? "soot" (first black_things)))
($check (eqv? "licorice" (second black_things)))
($check (eqv? "midnight" (third black_things)))
($check (eqv? "some chalkboards" (fourth black_things)))
($check (eqv? "tabbed\tthing" (fifth black_things)))
($check (eqv? "slish\\slash" (last black_things)))
