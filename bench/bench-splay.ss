;; splay: top-down splay tree via recursive path-copy. Allocation- and
;; pointer-heavy; non-tail recursive on tree depth. Insert N deterministic
;; keys, then look up the same N keys. Output: hit count (== N).

(define (node k l r) (cons k (cons l r)))
(define (nk n) (car n))
(define (nl n) (car (cdr n)))
(define (nr n) (cdr (cdr n)))

;; Handle the "key < t.key" case (l is non-nil, caller guarantees). When
;; we can't descend further (ll/lr nil), still rotate l up to root (zig)
;; -- that's what makes splay's partition invariant hold for tree-insert
;; when the key isn't in the tree.
(define (splay-left t key)
  (let ((k (nk t)) (l (nl t)) (r (nr t)))
    (let ((lk (nk l)) (ll (nl l)) (lr (nr l)))
      (cond
        ((= key lk) (node lk ll (node k lr r)))
        ((< key lk)
         (if (null? ll)
             (node lk ll (node k lr r))
             (let ((s (splay ll key)))
               (node (nk s) (nl s)
                     (node lk (nr s) (node k lr r))))))
        (else
         (if (null? lr)
             (node lk ll (node k lr r))
             (let ((s (splay lr key)))
               (node (nk s)
                     (node lk ll (nl s))
                     (node k (nr s) r)))))))))

;; Handle the "key > t.key" case (r is non-nil, caller guarantees).
(define (splay-right t key)
  (let ((k (nk t)) (l (nl t)) (r (nr t)))
    (let ((rk (nk r)) (rl (nl r)) (rr (nr r)))
      (cond
        ((= key rk) (node rk (node k l rl) rr))
        ((> key rk)
         (if (null? rr)
             (node rk (node k l rl) rr)
             (let ((s (splay rr key)))
               (node (nk s)
                     (node rk (node k l rl) (nl s))
                     (nr s)))))
        (else
         (if (null? rl)
             (node rk (node k l rl) rr)
             (let ((s (splay rl key)))
               (node (nk s)
                     (node k l (nl s))
                     (node rk (nr s) rr)))))))))

(define (splay t key)
  (cond
    ((null? t) t)
    ((= key (nk t)) t)
    ((< key (nk t)) (if (null? (nl t)) t (splay-left t key)))
    (else (if (null? (nr t)) t (splay-right t key)))))

(define (tree-insert t key)
  (if (null? t)
      (node key '() '())
      (let ((s (splay t key)))
        (cond
          ((= (nk s) key) s)
          ((< key (nk s)) (node key (nl s) (node (nk s) '() (nr s))))
          (else (node key (node (nk s) (nl s) '()) (nr s)))))))

;; Mutable root; each op replaces the tree with the result.
(define tree '())

;; Deterministic key sequence: (i * 31) mod N is a permutation of 0..N-1
;; when N is coprime with 31 (N = 10000 is).
(define N 10000)
(define (gen-key i) (modulo (* i 31) N))

(define (do-inserts i)
  (if (> i N)
      'done
      (begin
        (set! tree (tree-insert tree (gen-key i)))
        (do-inserts (+ i 1)))))

(define (do-finds i hits)
  (if (> i N)
      hits
      (begin
        (set! tree (splay tree (gen-key i)))
        (do-finds (+ i 1)
                  (if (and (pair? tree) (= (nk tree) (gen-key i)))
                      (+ hits 1)
                      hits)))))

(do-inserts 1)
(displayn (do-finds 1 0))
