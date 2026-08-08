;; Hash sets and hash maps: construction, predicates, key semantics, ref/setf!.

;; Construction, empty and initialized.
(define empty-map (hashmap))
(define empty-set (hashset))
($check (hashmap? empty-map))
($check (hashset? empty-set))
($check (= 0 (hashset-length empty-set)))

(define m (hashmap 'a 1 'b 2))
($check (= 1 (ref m 'a)))
($check (= 2 (ref m 'b)))

(define s (hashset 'a 'b))
($check (= 2 (hashset-length s)))
($check (ref s 'a))
($check (not (ref s 'c)))

;; Predicates don't confuse the two types, or unrelated values.
($check (not (hashmap? empty-set)))
($check (not (hashset? empty-map)))
($check (not (hashmap? '())))
($check (not (hashmap? (vector 1))))
($check (not (hashset? "hashset")))
($check (not (hashset? (tuple 1))))

;; Duplicate keys collapse; for a map the last value wins.
($check (= 3 (ref (hashmap 'k 1 'k 2 'k 3) 'k)))
($check (ref (hashset 'k 'k) 'k))
($check (= 1 (hashset-length (hashset 'k 'k))))

;; setf! inserts and overwrites.
(define counters (hashmap))
(setf! counters 'hits 1)
($check (= 1 (ref counters 'hits)))
(setf! counters 'hits 2)
($check (= 2 (ref counters 'hits)))

(define seen (hashset))
(setf! seen 'x #t)
($check (ref seen 'x))

;; Removal ignores absent elements and permits later reinsertion.
(define removable-set (hashset 'kept 'removed))
(hashset-unset! removable-set 'missing)
($check (ref removable-set 'kept))
($check (ref removable-set 'removed))
(hashset-unset! removable-set 'removed)
($check (= 1 (hashset-length removable-set)))
($check (ref removable-set 'kept))
($check (not (ref removable-set 'removed)))
(setf! removable-set 'removed #t)
($check (= 2 (hashset-length removable-set)))
($check (ref removable-set 'removed))

;; #f is a storable value, distinct from absence.
(define flags (hashmap 'off #f))
($check (eq? #f (ref flags 'off)))

;; Strings are keys by content, not identity.
(define by-name (hashmap "alpha" 1))
(define alpha (string-append "al" "pha"))
($check (not (eq? alpha "alpha")))
($check (= 1 (ref by-name alpha)))
(setf! by-name alpha 9)
($check (= 9 (ref by-name "alpha")))

;; Numbers compare by value, and the two zeroes are one key.
(define numbers (hashmap 1 'one 2.5 'two-and-a-half -0.0 'zero))
($check (eq? 'one (ref numbers 1)))
($check (eq? 'one (ref numbers 1.0)))
($check (eq? 'two-and-a-half (ref numbers 2.5)))
($check (eq? 'zero (ref numbers 0.0)))
($check (eq? 'zero (ref numbers -0.0)))

;; Booleans, characters, and the empty list are keys.
(define mixed (hashmap #t 'yes #f 'no #\a 'letter '() 'nil))
($check (eq? 'yes (ref mixed #t)))
($check (eq? 'no (ref mixed #f)))
($check (eq? 'letter (ref mixed #\a)))
($check (eq? 'nil (ref mixed '())))

;; Symbols are keys by interned identity, however they were built.
(define symbols (hashmap 'alpha 1))
($check (= 1 (ref symbols (string->symbol "alpha"))))

;; Tuples are keys structurally, including nested tuples.
(define points (hashmap (tuple 1 2) 'origin-ish))
($check (eq? 'origin-ish (ref points (tuple 1 2))))
($check (not (eq? (tuple 1 2) (tuple 1 2))))
(define nested (hashmap (tuple (tuple 'a 'b) 'c) 'deep))
($check (eq? 'deep (ref nested (tuple (tuple 'a 'b) 'c))))
;; Tuple keys that differ stay distinct.
(setf! points (tuple 2 1) 'other)
($check (eq? 'origin-ish (ref points (tuple 1 2))))
($check (eq? 'other (ref points (tuple 2 1))))
;; Tuple element types participate in the key.
(define tagged (hashmap (tuple "a" 1) 'string-a (tuple 'a 1) 'symbol-a))
($check (eq? 'string-a (ref tagged (tuple "a" 1))))
($check (eq? 'symbol-a (ref tagged (tuple 'a 1))))

;; Constant-key and register-key access agree.
(define (ref-by-register table key) (ref table key))
(define (set-by-register table key value) (setf! table key value))
(define constant-vs-register (hashmap 'a 1))
($check (= (ref constant-vs-register 'a) (ref-by-register constant-vs-register 'a)))
(set-by-register constant-vs-register 'b 2)
($check (= 2 (ref constant-vs-register 'b)))
(setf! constant-vs-register 'c 3)
($check (= 3 (ref-by-register constant-vs-register 'c)))

;; The same call site works after the receiver type changes.
(define v (vector 10 20))
($check (= 10 (ref-by-register v 0)))
($check (= 1 (ref-by-register constant-vs-register 'a)))
($check (= 20 (ref-by-register v 1)))

;; Growth: enough insertions to rehash several times.
(define big (hashmap))
(define big-set (hashset))
(let loop ((i 0))
  (if (< i 1000)
      (begin
        (setf! big i (* i i))
        (setf! big-set (tuple i 'k) #t)
        (loop (+ i 1)))))
(let loop ((i 0))
  (if (< i 1000)
      (begin
        ($check (= (* i i) (ref big i)))
        ($check (ref big-set (tuple i 'k)))
        (loop (+ i 1)))))
($check (not (ref big-set (tuple 1000 'k))))

;; GC retention: keys and values reachable only from the table survive collection.
(define retained (hashmap))
(setf! retained (tuple "key" 1) (list 'value (vector 1 2 3)))
(let loop ((i 0))
  (if (< i 20000)
      (begin
        (vector 1 2 3 4 5 6 7 8)
        (loop (+ i 1)))))
($check (equal? (list 'value (vector 1 2 3)) (ref retained (tuple "key" 1))))
($check (= (* 999 999) (ref big 999)))

;; equal? is structural and order-independent.
($check (equal? (hashmap 'a 1 'b 2) (hashmap 'b 2 'a 1)))
($check (equal? (hashset 'a 'b) (hashset 'b 'a)))
($check (equal? (hashmap) (hashmap)))
($check (not (equal? (hashmap 'a 1) (hashmap 'a 2))))
($check (not (equal? (hashmap 'a 1) (hashmap 'b 1))))
($check (not (equal? (hashmap 'a 1) (hashmap 'a 1 'b 2))))
($check (not (equal? (hashset 'a) (hashset 'a 'b))))
($check (not (equal? (hashmap 'a 1) (hashset 'a))))
($check (not (equal? (hashmap 'a 1) 'a)))
($check (not (equal? (hashset 'a) (list 'a))))
;; Values compare recursively; keys compare structurally.
($check (equal? (hashmap 'a (list 1 (vector 2))) (hashmap 'a (list 1 (vector 2)))))
($check (equal? (hashmap (tuple 1 "x") 'v) (hashmap (tuple 1 "x") 'v)))

;; A table is a value like any other: it can be stored, but not used as a key.
(define nesting (hashmap 'inner (hashmap 'leaf 42)))
($check (= 42 (ref (ref nesting 'inner) 'leaf)))
($check (hashmap? (ref nesting 'inner)))

;; #hashset(...) and #hashmap(...) reader syntax: elements are data.
($check (equal? (hashset 'a 'b) #hashset(a b)))
($check (ref #hashset(a b) 'b))
($check (not (ref #hashset(a b) 'c)))
($check (equal? (hashmap 'a 1 'b 2) #hashmap(a 1 b 2)))
($check (= 2 (ref #hashmap(a 1 b 2) 'b)))
($check (hashset? #hashset()))
($check (hashmap? #hashmap()))
(define k 'key)
($check (= 5 (ref `#hashmap(,k 5) 'key)))
($check (ref `#hashset(,k) 'key))

;; NaN is one key.
(define with-nan (hashmap (/ 0 0) 'nan-value))
($check (eq? 'nan-value (ref with-nan (/ 0 0))))
($check (eq? 'nan-value (ref with-nan (sqrt -1))))
(setf! with-nan (- (/ 1 0) (/ 1 0)) 'replaced)
($check (eq? 'replaced (ref with-nan (/ 0 0))))
