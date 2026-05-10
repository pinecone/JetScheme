;; Quicksort

(define (qsort ls)
  (if (null? ls)
      ls
      (let ((pivot (first ls)))
        (append (qsort (filter (lambda (x) (> pivot x)) ls))
                (filter (lambda (x) (= pivot x)) ls)
                (qsort (filter (lambda (x) (< pivot x)) ls))))))

(define sorted (qsort '(5 3 8 1 9 2 7 4 6 0)))
($check (equal? '(0 1 2 3 4 5 6 7 8 9) sorted))
($check (= 10 (length sorted)))
