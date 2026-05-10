;; File I/O
(define x (open-input-file "port_file.ss"))
;; First char of this file is ';'
($check (eqv? #\; (read-char x)))
(close-input-port x)
