;; Struct types: definition, identity, predicate, construction, ref/set.

(define point (struct 'point '(x y)))
(define point2 (struct 'point '(x y)))

;; Each (struct ...) call creates a fresh, distinct type — even with same name.
($check (not (eq? point point2)))

;; The bare type isn't an instance of itself.
($check (not (isa? point point)))
($check (not (isa? 5 point)))
($check (not (isa? "hi" point)))
($check (not (isa? '() point)))

;; Construction: calling a type with N args makes an instance.
(define p (point 1 2))
($check (isa? p point))

;; Construction yields a fresh instance each call.
(define q (point 1 2))
($check (not (eq? p q)))

;; An instance of point isn't an instance of a same-named, separately-defined type.
(define r (point2 1 2))
($check (isa? r point2))
($check (not (isa? r point)))
($check (not (isa? p point2)))

;; Different field counts → different types.
(define line (struct 'line '(start end mid)))
(define ln (line 0 10 5))
($check (isa? ln line))
($check (not (isa? ln point)))
($check (not (isa? p line)))

;; Field access via ref.
(define pp (point 7 8))
($check (= 7 (ref pp 'x)))
($check (= 8 (ref pp 'y)))
(define x-key (string->symbol "x"))
($check (= 7 (ref pp x-key)))

;; Mutation via setf!.
(setf! pp 'x 100)
(setf! pp 'y 200)
($check (= 100 (ref pp 'x)))
($check (= 200 (ref pp 'y)))
(setf! pp x-key 101)
($check (= 101 (ref pp 'x)))

;; Mutation on one instance doesn't affect another.
(define pp2 (point 7 8))
(setf! pp2 'x 9999)
($check (= 101 (ref pp 'x)))
($check (= 9999 (ref pp2 'x)))

;; A constant-key access site must recover when the receiver shape changes.
(define alternate-point (struct 'alternate-point '(x z)))
(define ap (alternate-point 300 400))
(define (read-xs objects)
  (if (null? objects)
      '()
      (cons (ref (car objects) 'x) (read-xs (cdr objects)))))
($check (equal? '(101 300 9999) (read-xs (list pp ap pp2))))
(define (write-xs! objects values)
  (if (null? objects)
      #t
      (begin
        (setf! (car objects) 'x (car values))
        (write-xs! (cdr objects) (cdr values)))))
(write-xs! (list pp ap pp2) '(11 22 33))
($check (equal? '(11 22 33) (read-xs (list pp ap pp2))))

;; All field types are atoms — strings, lists, other structs all work.
(define box-t (struct 'box '(label contents)))
(define b (box-t "thing" (list 1 2 3)))
($check (equal? "thing" (ref b 'label)))
($check (equal? '(1 2 3) (ref b 'contents)))

;; Structs in structs.
(define b2 (box-t 'outer b))
($check (eq? b (ref b2 'contents)))
($check (equal? "thing" (ref (ref b2 'contents) 'label)))
