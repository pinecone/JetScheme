;; Test include form

(include "inc_helper.ss")

;; Definitions from included file are visible
($check (= 42 inc-value))

;; $file reports this file, not the included file
($check (eqv? "include.ss" ($file)))
