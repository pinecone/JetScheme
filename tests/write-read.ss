;; write/read round trip of every datum kind through a file port

(define out (open-output-file "/tmp/jet_write_read.txt"))
(define datums
  (list 42 -3.5 #t #f #\a #\newline 'sym "str with \"esc\"\n" '()
        '(1 2 3) '(a . b) '(a (b c) d) #(1 #(2) three) #u8(0 127 255)
        #tuple(1 two #tuple(3)) #hashset(a b) #hashmap(k 5)))
(for-each (lambda (d) (write d out) (newline out)) datums)
(close-output-port out)

(define in (open-input-file "/tmp/jet_write_read.txt"))
(for-each (lambda (d) ($check (equal? d (read in)))) datums)
($check (eof-object? (read in)))
(close-input-port in)

;; display and write to a port match their stdout forms
(define out2 (open-output-file "/tmp/jet_write_read2.txt"))
(display "x y" out2)
(newline out2)
(displayn 7 out2)
(close-output-port out2)
(define in2 (open-input-file "/tmp/jet_write_read2.txt"))
($check (eq? 'x (read in2)))
($check (eq? 'y (read in2)))
($check (= 7 (read in2)))
($check (eof-object? (read in2)))
(close-input-port in2)
