;; Test include form

(include "inc-helper.ss")

;; Definitions from included file are visible
($check (= 42 inc-value))

;; $file reports this file, not the included file
($check (string=? "include.ss" ($file)))

;; Nested include: paths resolve relative to the including file
(include "inc/nested-a.ss")
($check (= 10 nested-b))
($check (= 11 nested-a))
