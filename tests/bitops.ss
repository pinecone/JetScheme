($check (= 0 (bitwise-and 5 2)))
($check (= 5 (bitwise-and 7 5)))
($check (= -1 (bitwise-and)))

($check (= 5 (bitwise-ior 1 4)))
($check (= 7 (bitwise-ior 1 2 4)))
($check (= 0 (bitwise-ior)))

($check (= 0 (bitwise-xor 5 5)))
($check (= 7 (bitwise-xor 1 2 4)))
($check (= 53257 (bitwise-xor 1 53256)))
($check (= 0 (bitwise-xor)))

($check (= -1 (bitwise-not 0)))
($check (= -6 (bitwise-not 5)))
($check (= 0 (bitwise-not -1)))

($check (= 16 (arithmetic-shift 1 4)))
($check (= 8 (arithmetic-shift 32 -2)))
($check (= -4 (arithmetic-shift -8 -1)))
($check (= 5 (arithmetic-shift 5 0)))

;; 32-bit truncation: high bits dropped (JS-style).
($check (= 0 (bitwise-and 4294967296 -1)))   ;; 2^32 truncates to 0
($check (= 1 (bitwise-and 4294967297 1)))    ;; 2^32 + 1 -> 1

(displayn "all bitops checks passed")
;; Lowered register calls agree with the first-class primitive's 32-bit semantics.
(define (check-shift-case value count expected)
  ($check (eq? expected (arithmetic-shift value count)))
  ($check (eq? expected (apply arithmetic-shift (list value count)))))
(check-shift-case 9.75 1.75 18)
(check-shift-case -9.75 -1.75 -5)
(check-shift-case 4294967296 0 0)
(check-shift-case 4294967297 0 1)
(check-shift-case 4294967295 -1 -1)
(check-shift-case 2147483648 -1 -1073741824)
(check-shift-case 1 31 -2147483648)
(check-shift-case -1 32 -1)
(check-shift-case 5 -32 5)
(check-shift-case 1 -2147483648 1)
(check-shift-case 1 2147483648 1)
(check-shift-case 1 4294967297 2)
(check-shift-case 1 -4294967297 0)
(check-shift-case -0.0 -16 0)
;; Immediate counts wrap through int32 and mask to 5 bits.
(check-shift-case -9.75 2147483648 -9)
(check-shift-case -9.75 -2147483648 -9)
(check-shift-case -9.75 2147483649 -1)
(check-shift-case -9.75 -2147483649 -2147483648)
(check-shift-case -9.75 4294967297 -18)
(check-shift-case -9.75 -4294967297 -5)

;; Immediate shifts can consume and produce the threaded unboxed value.
(define (shift-chain value)
  (+ (arithmetic-shift (+ value 0.75) -1) 0.5))
($check (eq? 5.5 (shift-chain 9.25)))
($check (eq? -3.5 (shift-chain -9.25)))

;; A computed count can be the unboxed right operand.
(define (shift-count-chain count)
  (arithmetic-shift 1024 (+ count 0.75)))
($check (eq? 2048 (shift-count-chain 0.25)))
($check (eq? 512 (shift-count-chain -1.75)))

;; Shift output can feed the right operand of the next arithmetic operation.
(define (shift-result-chain value)
  (+ 0.5 (arithmetic-shift value -1)))
($check (eq? 4.5 (shift-result-chain 8)))

;; A stored result remains canonical when used more than once.
(let ((value (arithmetic-shift -0.75 -16)))
  ($check (eq? 0 value))
  ($check (eq? 0 (+ value value))))
;; Immediate chains exercise the ashf immediate modes with wrapped counts.
(define (shift-chain-33 value) (+ (arithmetic-shift (+ value 0.75) 33) 0.5))
(define (shift-chain-neg33 value) (+ (arithmetic-shift (+ value 0.75) -33) 0.5))
(define (shift-chain-big value) (+ (arithmetic-shift (+ value 0.75) -2147483648) 0.5))
($check (eq? -15.5 (shift-chain-33 -9.25)))
($check (eq? -3.5 (shift-chain-neg33 -9.25)))
($check (eq? -7.5 (shift-chain-big -9.25)))
($check (eq? -18 (arithmetic-shift -9.75 33)))
($check (eq? -5 (arithmetic-shift -9.75 -33)))
($check (eq? -9 (arithmetic-shift -9.75 2147483648)))
($check (eq? -9 (arithmetic-shift -9.75 -2147483648)))
($check (eq? -18 (arithmetic-shift -9.75 4294967297)))
($check (eq? -1 (arithmetic-shift -9.75 2147483649)))
($check (eq? -2147483648 (arithmetic-shift -9.75 -2147483649)))
($check (eq? -5 (arithmetic-shift -9.75 -4294967297)))

;; A same-named Scheme binding must not lower to the primitive instruction.
(let ((arithmetic-shift (lambda (value count) 99)))
  ($check (= 99 (arithmetic-shift 1 2))))

;; Shift counts mask to 5 bits, negative counts shift right.
($check (= 2 (arithmetic-shift 1 33)))
($check (= 0 (arithmetic-shift 1 -33)))
($check (= -16 (arithmetic-shift -8 1)))
($check (= -4 (arithmetic-shift -8 -1)))

;; Single-argument folds return the value.
($check (= 5 (bitwise-and 5)))
($check (= 5 (bitwise-ior 5)))
($check (= 5 (bitwise-xor 5)))
($check (= 0 (bitwise-not -1)))
