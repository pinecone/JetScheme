;; File I/O
(define x (open-input-file "port-file.ss"))
;; First char of this file is ';'
($check (eqv? #\; (read-char x)))
($check (input-port? x))
($check (not (output-port? x)))
(close-input-port x)

;; predicates reject non-ports
($check (not (input-port? 42)))
($check (not (output-port? "hi")))

;; output port
(define tmp (open-output-file "/tmp/jet_port_test.txt"))
($check (output-port? tmp))
($check (not (input-port? tmp)))
(write-char #\H tmp)
(write-char #\i tmp)
(close-output-port tmp)

(define back (open-input-file "/tmp/jet_port_test.txt"))
($check (eqv? #\H (read-char back)))
($check (eqv? #\i (read-char back)))
(close-input-port back)
