;; List benchmark - builds and traverses lists, heavy on cons/car/cdr
(define (build-list n)
  (if (zero? n)
      '()
      (cons n (build-list (- n 1)))))

(define (sum-list ls)
  (if (null? ls)
      0
      (+ (car ls) (sum-list (cdr ls)))))

(define (repeat f n arg)
  (if (zero? n)
      arg
      (begin
        (f arg)
        (repeat f n (- arg 1)))))

;; Build and sum a 1000-element list, 50 times
(define (bench n)
  (if (zero? n)
      0
      (begin
        (sum-list (build-list 1000))
        (bench (- n 1)))))

(displayn (bench 50))
