;; Included by include.ss; includes nested-b.ss relative to this directory.
(include "nested-b.ss")
(define nested-a (+ nested-b 1))
