;; File I/O
($check (not (open-input-file/maybe "/tmp/jet_port_missing.txt")))
(define maybe-port (open-input-file/maybe "port-file.ss"))
($check (input-port? maybe-port))
(close-input-port maybe-port)

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

;; read-bytes/all takes every remaining byte
(define whole (open-input-file "/tmp/jet_port_test.txt"))
(define both (read-bytes/all whole))
($check (bytevector? both))
($check (= 2 (bytevector-length both)))
($check (= 72 (ref both 0)))
($check (= 105 (ref both 1)))
($check (= 0 (bytevector-length (read-bytes/all whole))))
(close-input-port whole)

(define rest-only (open-input-file "/tmp/jet_port_test.txt"))
($check (eqv? #\H (read-char rest-only)))
($check (equal? (bytevector 105) (read-bytes/all rest-only)))
(close-input-port rest-only)

;; write-bytes stores every byte, including ones above 127
(define raw (open-output-file "/tmp/jet_port_bytes.bin"))
(write-bytes (bytevector 0 127 128 255) raw)
(close-output-port raw)

(define rawback (open-input-file "/tmp/jet_port_bytes.bin"))
(define stored (read-bytes/all rawback))
($check (= 4 (bytevector-length stored)))
($check (equal? (bytevector 0 127 128 255) stored))
(close-input-port rawback)
