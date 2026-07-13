;; Bytevector operations (R7RS-small subset)

;; literal syntax and predicate
($check (bytevector? #u8(0 10 255)))
($check (not (bytevector? #(1 2 3))))
($check (not (bytevector? "abc")))
($check (not (bytevector? 5)))
($check (not (bytevector? '())))

;; bytevector constructor
($check (equal? #u8(1 3 5 1 3 5) (bytevector 1 3 5 1 3 5)))
($check (equal? #u8() (bytevector)))
($check (bytevector? (bytevector 0)))

;; make-bytevector (2-arg only)
($check (equal? #u8(12 12) (make-bytevector 2 12)))
($check (equal? #u8(0 0 0 0) (make-bytevector 4 0)))
($check (equal? #u8() (make-bytevector 0 0)))

;; bytevector-length
($check (= 0 (bytevector-length #u8())))
($check (= 3 (bytevector-length #u8(1 2 3))))
($check (= 1 (bytevector-length (bytevector 255))))

;; bytevector-u8-ref
($check (= 8 (bytevector-u8-ref #u8(1 1 2 3 5 8 13 21) 5)))
($check (= 0 (bytevector-u8-ref #u8(0 10 5) 0)))
($check (= 5 (bytevector-u8-ref #u8(0 10 5) 2)))
($check (= 255 (bytevector-u8-ref (bytevector 255) 0)))

;; bytevector-u8-set!
(define bv (bytevector 1 2 3 4))
(bytevector-u8-set! bv 1 200)
($check (= 200 (bytevector-u8-ref bv 1)))
($check (= 1 (bytevector-u8-ref bv 0)))
($check (= 3 (bytevector-u8-ref bv 2)))
($check (= 4 (bytevector-u8-ref bv 3)))

;; ref / setf! fast path on bytevectors
(define bv2 (bytevector 10 20 30 40))
($check (= 10 (ref bv2 0)))
($check (= 40 (ref bv2 3)))
(setf! bv2 1 99)
($check (= 99 (ref bv2 1)))
($check (= 99 (bytevector-u8-ref bv2 1)))
;; ref returns a number, not a character
($check (number? (ref bv2 0)))
($check (not (char? (ref bv2 0))))

;; hot loop via ref fast path
(define hot (make-bytevector 100 0))
(define i 0)
(let loop ()
  (when (< i 100)
    (setf! hot i (modulo i 256))
    (set! i (+ i 1))
    (loop)))
(define sum 0)
(set! i 0)
(let loop ()
  (when (< i 100)
    (set! sum (+ sum (ref hot i)))
    (set! i (+ i 1))
    (loop)))
;; sum of i for i in [0..100) = 4950
($check (= 4950 sum))

;; bytevector-copy (3-arg: bv start end)
($check (equal? #u8(3 4) (bytevector-copy #u8(1 2 3 4 5) 2 4)))
($check (equal? #u8(1 2 3 4 5) (bytevector-copy #u8(1 2 3 4 5) 0 5)))
($check (equal? #u8() (bytevector-copy #u8(1 2 3) 1 1)))

;; bytevector-copy! (5-arg: to at from start end)
(define dst (bytevector 10 20 30 40 50))
(bytevector-copy! dst 1 #u8(1 2) 0 2)
($check (equal? #u8(10 1 2 40 50) dst))

;; bytevector-copy! overlap (forward shift within same bytevector)
(define ov (bytevector 1 2 3 4 5))
(bytevector-copy! ov 2 ov 0 3)
($check (equal? #u8(1 2 1 2 3) ov))

;; bytevector-append
($check (equal? #u8(0 1 2 3 4 5) (bytevector-append #u8(0 1 2) #u8(3 4 5))))
($check (equal? #u8() (bytevector-append)))
($check (equal? #u8(1 2) (bytevector-append #u8() #u8(1 2) #u8())))

;; display round-trip
($check (equal? #u8(0 10 255) (bytevector 0 10 255)))
($check (equal? #u8() (bytevector)))

;; apply ref routes through slow primitive fallback
($check (= 99 (apply ref (list bv2 1))))
