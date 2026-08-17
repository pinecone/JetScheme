;; String operations

;; append
(define x "h")
(define y "amburger")
($check (equal? "hamburger hamburger" (string-append x y " " x y)))

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

;; copy
($check (string=? "ell" (substring "hello" 1 4)))
($check (string=? "hello" (substring "hello" 0)))
($check (string=? "lo" (substring "hello" 3)))
($check (string=? "hello" (string-copy "hello")))
($check (string=? "ell" (string-copy "hello" 1 4)))

;; comparison
($check (string=? "abc" "abc"))
($check (not (string=? "abc" "abd")))
($check (string=? "x" "x" "x"))
($check (string<? "abc" "abd"))
($check (string<=? "abc" "abc"))
($check (string>? "abd" "abc"))
($check (string>=? "abc" "abc"))

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

;; Display and string values
(define black_things '("soot" "licorice" "midnight" "some chalkboards"
                       "tabbed\tthing" "before newline\nafter newline"
                       "\"quoted thing\"" "slish\\slash"))

($check (string=? "soot" (first black_things)))
($check (string=? "licorice" (second black_things)))
($check (string=? "midnight" (third black_things)))
($check (string=? "some chalkboards" (fourth black_things)))
($check (string=? "tabbed\tthing" (fifth black_things)))
($check (string=? "slish\\slash" (last black_things)))

;; UTF-8 is byte-level: length, indexing, and slicing count bytes
(define accented "héllo")
($check (= 6 (string-length accented)))
($check (= 104 (char->integer (string-ref accented 0))))
($check (= 195 (char->integer (string-ref accented 1))))
($check (= 169 (char->integer (string-ref accented 2))))
($check (= 108 (char->integer (ref accented 3))))
($check (string=? "hé" (substring accented 0 3)))
($check (string=? (string (integer->char 195)) (substring accented 1 2)))
($check (= 1 (string-length (substring accented 1 2))))
($check (string=? accented (string-copy accented)))

(define cjk "日本")
($check (= 6 (string-length cjk)))
($check (= 230 (char->integer (string-ref cjk 0))))
($check (string=? "日" (substring cjk 0 3)))
($check (= 12 (string-length (string-append cjk cjk))))
($check (string=? "日本日本" (string-append cjk cjk)))

;; comparison sorts by codepoint, so a multibyte character sorts above ASCII
($check (string<? "a" "é"))
($check (string<? "z" "日"))
($check (string<? "é" "日"))
($check (not (string=? "é" "e")))

;; escape sequences
($check (= 7 (char->integer (string-ref "\a" 0))))
($check (= 8 (char->integer (string-ref "\b" 0))))
($check (= 10 (char->integer (string-ref "\n" 0))))
($check (= 13 (char->integer (string-ref "\r" 0))))
($check (= 9 (char->integer (string-ref "\t" 0))))
($check (string=? "\\" (string (integer->char 92))))
($check (string=? "\"" (string (integer->char 34))))
($check (string=? "|" "\|"))

;; \x names a Unicode scalar value and stores its UTF-8 bytes
($check (string=? "A" "\x41;"))
($check (string=? "A" "\x0041;"))
($check (string=? "é" "\xe9;"))
($check (= 2 (string-length "\xe9;")))
($check (string=? "日本" "\x65e5;\x672c;"))
($check (= 5 (string-length "\x1f600;\x41;")))
($check (string=? "aAb" "a\x41;b"))
($check (= 1 (string-length "\x0;")))
($check (= 0 (char->integer (string-ref "\x0;" 0))))
($check (= 3 (string-length "a\x0;b")))
($check (= 0 (char->integer (string-ref "a\x0;b" 1))))
($check (string=? "a\x0;b" (string #\a #\null #\b)))

;; a backslash before a newline drops the newline and the following indent
($check (string=? "ab" "a\
                        b"))
($check (string=? "a b" "a \
                         b"))

;; Number parsing edge cases
($check (eq? (/ 0 0) (string->number "nan")))
($check (eq? (/ 1 0) (string->number "inf")))
($check (eq? (/ 1 0) (string->number "1e309")))
($check (= 0 (string->number "-0.0")))

;; multibyte UTF-8: strings are byte-level
(define lambda-str "λ")
($check (= 2 (string-length lambda-str)))
($check (= 4 (string-length "🎈")))
($check (= 206 (char->integer (string-ref lambda-str 0))))
($check (= 187 (char->integer (string-ref lambda-str 1))))
($check (string=? "λ" (substring "aλb" 1 3)))
($check (= 3 (string-length (string-append "a" lambda-str))))

;; codepoint ordering through UTF-8 byte comparison
($check (string<? "A" "λ"))
($check (string<? "λ" "🎈"))
($check (string<? "z" "é"))

($check (char=? (ref "ab" 1 :default #\z) #\b))
($check (char=? (ref "ab" 9 :default #\z) #\z))
