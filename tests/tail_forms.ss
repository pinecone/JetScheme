;; Tail position must be preserved through every surface form, so 50k-deep
;; recursion runs without stack overflow.

;; cond.
(define (cloop n)
  (cond ((zero? n) 'done)
        (else (cloop (- n 1)))))
($check (eq? 'done (cloop 50000)))

;; when.
(define wn 0)
(define (wloop n)
  (when (> n 0)
    (set! wn (+ wn 1))
    (wloop (- n 1))))
(wloop 50000)
($check (= 50000 wn))

;; unless.
(define un 0)
(define (uloop n)
  (unless (zero? n)
    (set! un (+ un 1))
    (uloop (- n 1))))
(uloop 50000)
($check (= 50000 un))

;; and.
(define (aloop n)
  (if (zero? n) 'done
      (and #t #t (aloop (- n 1)))))
($check (eq? 'done (aloop 50000)))

;; or.
(define (oloop n)
  (or (zero? n) (oloop (- n 1))))
($check (oloop 50000))

;; or with 3+ args.
($check (eq? (or #f #f 'c)        'c))
($check (eq? (or #f 'b #f)        'b))
($check (eq? (or 'a #f #f)        'a))
($check (not (or #f #f #f #f)))
;; or with 3+ args closing over an outer parameter.
(define (or3 v) (or v v v))
($check (eq? 'x (or3 'x)))
($check (not (or3 #f)))
