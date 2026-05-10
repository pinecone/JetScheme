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
