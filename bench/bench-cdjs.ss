;; cdjs: aircraft collision detection. Port of JetStream2 cdjs.
;; 1000 aircraft, 18 frames, 1336 expected collisions. Stresses fp math,
;; RB-tree mutation, and pointer-chasing through nested-loop pair tests.

(define ITERS 1)
(define NUM-AIRCRAFT 1000)
(define NUM-FRAMES 18)
(define EXPECTED-COLLISIONS 1336)

;; -------------------------------------------------------------------
;; constants
;; -------------------------------------------------------------------

(define MIN-X 0)
(define MIN-Y 0)
(define MAX-X 1000)
(define MAX-Y 1000)
(define MIN-Z 0)
(define MAX-Z 10)
(define PROXIMITY-RADIUS 1)
(define GOOD-VOXEL-SIZE 2)

;; -------------------------------------------------------------------
;; helpers
;; -------------------------------------------------------------------

(define (compare-numbers a b)
  (cond ((= a b)  0)
        ((<  a b) -1)
        (else      1)))

;; -------------------------------------------------------------------
;; Vector2D
;; -------------------------------------------------------------------

(define vec2-t (struct 'vec2 '(x y)))

(define (vec2-plus  a b)
  (vec2-t (+ (ref a 'x) (ref b 'x)) (+ (ref a 'y) (ref b 'y))))
(define (vec2-minus a b)
  (vec2-t (- (ref a 'x) (ref b 'x)) (- (ref a 'y) (ref b 'y))))

(define (vec2-compare a b)
  (let ((r (compare-numbers (ref a 'x) (ref b 'x))))
    (if (= r 0) (compare-numbers (ref a 'y) (ref b 'y)) r)))

;; -------------------------------------------------------------------
;; Vector3D
;; -------------------------------------------------------------------

(define vec3-t (struct 'vec3 '(x y z)))

(define (vec3-plus a b)
  (vec3-t (+ (ref a 'x) (ref b 'x))
          (+ (ref a 'y) (ref b 'y))
          (+ (ref a 'z) (ref b 'z))))
(define (vec3-minus a b)
  (vec3-t (- (ref a 'x) (ref b 'x))
          (- (ref a 'y) (ref b 'y))
          (- (ref a 'z) (ref b 'z))))
(define (vec3-dot a b)
  (+ (* (ref a 'x) (ref b 'x))
     (* (ref a 'y) (ref b 'y))
     (* (ref a 'z) (ref b 'z))))
(define (vec3-sqr-mag a) (vec3-dot a a))
(define (vec3-mag     a) (sqrt (vec3-sqr-mag a)))
(define (vec3-times a c)
  (vec3-t (* (ref a 'x) c) (* (ref a 'y) c) (* (ref a 'z) c)))

;; -------------------------------------------------------------------
;; CallSign
;; -------------------------------------------------------------------

(define cs-t (struct 'cs '(s)))

(define (cs-compare a b)
  (let ((sa (ref a 's)) (sb (ref b 's)))
    (cond ((string=? sa sb)  0)
          ((string<? sa sb) -1)
          (else              1))))

;; -------------------------------------------------------------------
;; Motion + findIntersection
;; -------------------------------------------------------------------

(define motion-t (struct 'motion '(cs p1 p2)))

(define (motion-delta m) (vec3-minus (ref m 'p2) (ref m 'p1)))

(define (motion-find-intersection m1 m2)
  (let* ((init1 (ref m1 'p1))
         (init2 (ref m2 'p1))
         (vec1  (motion-delta m1))
         (vec2  (motion-delta m2))
         (r     PROXIMITY-RADIUS)
         (a (vec3-sqr-mag (vec3-minus vec2 vec1))))
    (cond
      ((not (= a 0))
       (let* ((b (* 2 (vec3-dot (vec3-minus init1 init2)
                                (vec3-minus vec1 vec2))))
              (c (+ (- (* r r))
                    (vec3-sqr-mag (vec3-minus init2 init1))))
              (discr (- (* b b) (* 4 a c))))
         (cond
           ((< discr 0) #f)
           (else
            (let* ((sq (sqrt discr))
                   (v1 (/ (- (- b) sq) (* 2 a)))
                   (v2 (/ (+ (- b) sq) (* 2 a))))
              (cond
                ((and (<= v1 v2)
                      (or (and (<= v1 1) (<= 1 v2))
                          (and (<= v1 0) (<= 0 v2))
                          (and (<= 0 v1) (<= v2 1))))
                 (let* ((v  (if (<= v1 0) 0 v1))
                        (r1 (vec3-plus init1 (vec3-times vec1 v)))
                        (r2 (vec3-plus init2 (vec3-times vec2 v)))
                        (result (vec3-times (vec3-plus r1 r2) 0.5))
                        (rx (ref result 'x))
                        (ry (ref result 'y))
                        (rz (ref result 'z)))
                   (if (and (>= rx MIN-X) (<= rx MAX-X)
                            (>= ry MIN-Y) (<= ry MAX-Y)
                            (>= rz MIN-Z) (<= rz MAX-Z))
                       result
                       #f)))
                (else #f)))))))
      (else
       (let ((dist (vec3-mag (vec3-minus init2 init1))))
         (cond
           ((<= dist r) (vec3-times (vec3-plus init1 init2) 0.5))
           (else #f)))))))

;; -------------------------------------------------------------------
;; Red-black tree (parameterized by a comparator)
;; -------------------------------------------------------------------

(define rbnode-t (struct 'rbnode '(key val left right parent color)))
(define rbtree-t (struct 'rbtree '(root cmp)))

(define (make-rbtree cmp) (rbtree-t #f cmp))
(define (make-rbnode k v) (rbnode-t k v #f #f #f 'red))

(define (tree-min n)
  (if (ref n 'left) (tree-min (ref n 'left)) n))

(define (node-successor n)
  (cond
    ((ref n 'right) (tree-min (ref n 'right)))
    (else
     (let loop ((x n) (y (ref n 'parent)))
       (cond
         ((not y) #f)
         ((eq? x (ref y 'right)) (loop y (ref y 'parent)))
         (else y))))))

(define (tree-find t k)
  (let ((cmp (ref t 'cmp)))
    (let loop ((cur (ref t 'root)))
      (cond
        ((not cur) #f)
        (else
         (let ((c (cmp k (ref cur 'key))))
           (cond
             ((= c 0) cur)
             ((< c 0) (loop (ref cur 'left)))
             (else    (loop (ref cur 'right))))))))))

(define (tree-get t k)
  (let ((n (tree-find t k)))
    (and n (ref n 'val))))

(define (tree-left-rotate! t x)
  (let ((y (ref x 'right))
        (xp (ref x 'parent)))
    (setf! (ref x 'right) (ref y 'left))
    (when (ref y 'left) (setf! (ref (ref y 'left) 'parent) x))
    (setf! (ref y 'parent) xp)
    (cond
      ((not xp) (setf! (ref t 'root) y))
      ((eq? x (ref xp 'left))  (setf! (ref xp 'left)  y))
      (else                    (setf! (ref xp 'right) y)))
    (setf! (ref y 'left) x)
    (setf! (ref x 'parent) y)
    y))

(define (tree-right-rotate! t y)
  (let ((x (ref y 'left))
        (yp (ref y 'parent)))
    (setf! (ref y 'left) (ref x 'right))
    (when (ref x 'right) (setf! (ref (ref x 'right) 'parent) y))
    (setf! (ref x 'parent) yp)
    (cond
      ((not yp) (setf! (ref t 'root) x))
      ((eq? y (ref yp 'left))  (setf! (ref yp 'left)  x))
      (else                    (setf! (ref yp 'right) x)))
    (setf! (ref x 'right) y)
    (setf! (ref y 'parent) x)
    x))

(define (tree-insert! t k v)
  ;; (cons #t new-node) on new, (cons #f old-val) on update.
  (let ((cmp (ref t 'cmp)))
    (let loop ((y #f) (x (ref t 'root)))
      (cond
        ((not x)
         (let ((z (make-rbnode k v)))
           (setf! (ref z 'parent) y)
           (cond
             ((not y) (setf! (ref t 'root) z))
             ((< (cmp k (ref y 'key)) 0) (setf! (ref y 'left) z))
             (else (setf! (ref y 'right) z)))
           (cons #t z)))
        (else
         (let ((c (cmp k (ref x 'key))))
           (cond
             ((< c 0) (loop x (ref x 'left)))
             ((> c 0) (loop x (ref x 'right)))
             (else
              (let ((old (ref x 'val)))
                (setf! (ref x 'val) v)
                (cons #f old))))))))))

(define (tree-put! t k v)
  (let ((res (tree-insert! t k v)))
    (cond
      ((not (car res)) (cdr res))
      (else
       (tree-put-fixup! t (cdr res))
       (setf! (ref (ref t 'root) 'color) 'black)
       #f))))

(define (tree-put-fixup! t x)
  (let loop ((x x))
    (cond
      ((and (not (eq? x (ref t 'root)))
            (eq? 'red (ref (ref x 'parent) 'color)))
       (let* ((p  (ref x 'parent))
              (gp (ref p 'parent)))
         (cond
           ((eq? p (ref gp 'left))
            (let ((y (ref gp 'right)))
              (cond
                ((and y (eq? 'red (ref y 'color)))
                 (setf! (ref p  'color) 'black)
                 (setf! (ref y  'color) 'black)
                 (setf! (ref gp 'color) 'red)
                 (loop gp))
                (else
                 (let ((x2 (cond
                             ((eq? x (ref p 'right))
                              (tree-left-rotate! t p)
                              p)
                             (else x))))
                   (setf! (ref (ref x2 'parent) 'color) 'black)
                   (setf! (ref (ref (ref x2 'parent) 'parent) 'color) 'red)
                   (tree-right-rotate! t (ref (ref x2 'parent) 'parent))
                   (loop x2))))))
           (else
            (let ((y (ref gp 'left)))
              (cond
                ((and y (eq? 'red (ref y 'color)))
                 (setf! (ref p  'color) 'black)
                 (setf! (ref y  'color) 'black)
                 (setf! (ref gp 'color) 'red)
                 (loop gp))
                (else
                 (let ((x2 (cond
                             ((eq? x (ref p 'left))
                              (tree-right-rotate! t p)
                              p)
                             (else x))))
                   (setf! (ref (ref x2 'parent) 'color) 'black)
                   (setf! (ref (ref (ref x2 'parent) 'parent) 'color) 'red)
                   (tree-left-rotate! t (ref (ref x2 'parent) 'parent))
                   (loop x2)))))))))
      (else #t))))

(define (tree-remove! t k)
  (let ((z (tree-find t k)))
    (cond
      ((not z) #f)
      (else
       (let* ((y (if (or (not (ref z 'left)) (not (ref z 'right)))
                     z
                     (node-successor z)))
              (x (if (ref y 'left) (ref y 'left) (ref y 'right)))
              (xparent
                (cond
                  (x
                   (setf! (ref x 'parent) (ref y 'parent))
                   (ref x 'parent))
                  (else (ref y 'parent)))))
         (cond
           ((not (ref y 'parent)) (setf! (ref t 'root) x))
           ((eq? y (ref (ref y 'parent) 'left))
            (setf! (ref (ref y 'parent) 'left) x))
           (else
            (setf! (ref (ref y 'parent) 'right) x)))
         (cond
           ((not (eq? y z))
            (when (eq? 'black (ref y 'color))
              (tree-remove-fixup! t x xparent))
            (setf! (ref y 'parent) (ref z 'parent))
            (setf! (ref y 'color)  (ref z 'color))
            (setf! (ref y 'left)   (ref z 'left))
            (setf! (ref y 'right)  (ref z 'right))
            (when (ref z 'left)
              (setf! (ref (ref z 'left)  'parent) y))
            (when (ref z 'right)
              (setf! (ref (ref z 'right) 'parent) y))
            (cond
              ((ref z 'parent)
               (cond
                 ((eq? z (ref (ref z 'parent) 'left))
                  (setf! (ref (ref z 'parent) 'left)  y))
                 (else
                  (setf! (ref (ref z 'parent) 'right) y))))
              (else (setf! (ref t 'root) y))))
           ((eq? 'black (ref y 'color))
            (tree-remove-fixup! t x xparent)))
         (ref z 'val))))))

(define (tree-remove-fixup! t x xparent)
  (let loop ((x x) (xparent xparent))
    (cond
      ((and (not (eq? x (ref t 'root)))
            (or (not x) (eq? 'black (ref x 'color))))
       (cond
         ((eq? x (ref xparent 'left))
          (let* ((w0 (ref xparent 'right))
                 (w (cond
                      ((eq? 'red (ref w0 'color))
                       (setf! (ref w0 'color) 'black)
                       (setf! (ref xparent 'color) 'red)
                       (tree-left-rotate! t xparent)
                       (ref xparent 'right))
                      (else w0))))
            (cond
              ((and (or (not (ref w 'left))
                        (eq? 'black (ref (ref w 'left) 'color)))
                    (or (not (ref w 'right))
                        (eq? 'black (ref (ref w 'right) 'color))))
               (setf! (ref w 'color) 'red)
               (loop xparent (ref xparent 'parent)))
              (else
               (let ((w2 (cond
                           ((or (not (ref w 'right))
                                (eq? 'black (ref (ref w 'right) 'color)))
                            (setf! (ref (ref w 'left) 'color) 'black)
                            (setf! (ref w 'color) 'red)
                            (tree-right-rotate! t w)
                            (ref xparent 'right))
                           (else w))))
                 (setf! (ref w2 'color) (ref xparent 'color))
                 (setf! (ref xparent 'color) 'black)
                 (when (ref w2 'right)
                   (setf! (ref (ref w2 'right) 'color) 'black))
                 (tree-left-rotate! t xparent)
                 (let ((nx (ref t 'root)))
                   (loop nx (and nx (ref nx 'parent)))))))))
         (else
          (let* ((w0 (ref xparent 'left))
                 (w (cond
                      ((eq? 'red (ref w0 'color))
                       (setf! (ref w0 'color) 'black)
                       (setf! (ref xparent 'color) 'red)
                       (tree-right-rotate! t xparent)
                       (ref xparent 'left))
                      (else w0))))
            (cond
              ((and (or (not (ref w 'right))
                        (eq? 'black (ref (ref w 'right) 'color)))
                    (or (not (ref w 'left))
                        (eq? 'black (ref (ref w 'left) 'color))))
               (setf! (ref w 'color) 'red)
               (loop xparent (ref xparent 'parent)))
              (else
               (let ((w2 (cond
                           ((or (not (ref w 'left))
                                (eq? 'black (ref (ref w 'left) 'color)))
                            (setf! (ref (ref w 'right) 'color) 'black)
                            (setf! (ref w 'color) 'red)
                            (tree-left-rotate! t w)
                            (ref xparent 'left))
                           (else w))))
                 (setf! (ref w2 'color) (ref xparent 'color))
                 (setf! (ref xparent 'color) 'black)
                 (when (ref w2 'left)
                   (setf! (ref (ref w2 'left) 'color) 'black))
                 (tree-right-rotate! t xparent)
                 (let ((nx (ref t 'root)))
                   (loop nx (and nx (ref nx 'parent)))))))))))
      (else
       (when x (setf! (ref x 'color) 'black))))))

(define (tree-for-each t f)
  (let ((root (ref t 'root)))
    (when root
      (let loop ((cur (tree-min root)))
        (when cur
          (f (ref cur 'key) (ref cur 'val))
          (loop (node-successor cur)))))))

;; -------------------------------------------------------------------
;; Voxel hashing + isInVoxel
;; -------------------------------------------------------------------

(define (voxel-hash pos)
  (let* ((vs GOOD-VOXEL-SIZE)
         (x (ref pos 'x))
         (y (ref pos 'y))
         (xd (truncate (/ x vs)))
         (yd (truncate (/ y vs)))
         (rx (* vs xd))
         (ry (* vs yd))
         (rx (if (< x 0) (- rx vs) rx))
         (ry (if (< y 0) (- ry vs) ry)))
    (vec2-t rx ry)))

(define (is-in-voxel voxel motion)
  (let ((vx (ref voxel 'x)) (vy (ref voxel 'y)))
    (cond
      ((or (> vx MAX-X) (< vx MIN-X) (> vy MAX-Y) (< vy MIN-Y)) #f)
      (else
       (let* ((init (ref motion 'p1))
              (fin  (ref motion 'p2))
              (vs GOOD-VOXEL-SIZE)
              (r (/ PROXIMITY-RADIUS 2))
              (x0 (ref init 'x)) (xv (- (ref fin 'x) x0))
              (y0 (ref init 'y)) (yv (- (ref fin 'y) y0))
              ;; placeholders when motion has no component along the axis
              (lx0 (if (= xv 0) 0 (/ (- vx r x0) xv)))
              (hx0 (if (= xv 0) 0 (/ (- (+ vx vs r) x0) xv)))
              (ly0 (if (= yv 0) 0 (/ (- vy r y0) yv)))
              (hy0 (if (= yv 0) 0 (/ (- (+ vy vs r) y0) yv)))
              (lo-x (if (< xv 0) hx0 lx0))
              (hi-x (if (< xv 0) lx0 hx0))
              (lo-y (if (< yv 0) hy0 ly0))
              (hi-y (if (< yv 0) ly0 hy0)))
         (and
          (or (and (= xv 0) (<= vx (+ x0 r)) (<= (- x0 r) (+ vx vs)))
              (and (not (= xv 0)) (<= lo-x 1) (<= 1 hi-x))
              (and (not (= xv 0)) (<= lo-x 0) (<= 0 hi-x))
              (and (not (= xv 0)) (<= 0 lo-x) (<= hi-x 1)))
          (or (and (= yv 0) (<= vy (+ y0 r)) (<= (- y0 r) (+ vy vs)))
              (and (not (= yv 0)) (<= lo-y 1) (<= 1 hi-y))
              (and (not (= yv 0)) (<= lo-y 0) (<= 0 hi-y))
              (and (not (= yv 0)) (<= 0 lo-y) (<= hi-y 1)))
          (or (= xv 0) (= yv 0)
              (and (<= lo-y hi-x) (<= hi-x hi-y))
              (and (<= lo-y lo-x) (<= lo-x hi-y))
              (and (<= lo-x lo-y) (<= hi-y hi-x)))))))))

;; -------------------------------------------------------------------
;; Voxel map mutation: each voxel maps to a box holding a motion list.
;; -------------------------------------------------------------------

(define box-t (struct 'box '(v)))

(define (draw-motion-on-voxel-map voxelmap motion)
  (let* ((seen (make-rbtree vec2-compare))
         (vs GOOD-VOXEL-SIZE)
         (horizontal (vec2-t vs 0))
         (vertical   (vec2-t 0 vs)))
    (let recurse ((nv (voxel-hash (ref motion 'p1))))
      (cond
        ((not (is-in-voxel nv motion)) #f)
        ((tree-put! seen nv #t) #f)
        (else
         (let ((bx (tree-get voxelmap nv)))
           (cond
             ((not bx)
              (tree-put! voxelmap nv (box-t (list motion))))
             (else
              (setf! (ref bx 'v) (cons motion (ref bx 'v))))))
         (recurse (vec2-minus nv horizontal))
         (recurse (vec2-plus  nv horizontal))
         (recurse (vec2-minus nv vertical))
         (recurse (vec2-plus  nv vertical))
         (recurse (vec2-minus (vec2-minus nv horizontal) vertical))
         (recurse (vec2-plus  (vec2-minus nv horizontal) vertical))
         (recurse (vec2-minus (vec2-plus  nv horizontal) vertical))
         (recurse (vec2-plus  (vec2-plus  nv horizontal) vertical)))))))

(define (reduce-collision-set motions)
  (let ((vm (make-rbtree vec2-compare)))
    (for-each (lambda (m) (draw-motion-on-voxel-map vm m)) motions)
    (let ((res '()))
      (tree-for-each vm
        (lambda (k bx)
          (let ((lst (ref bx 'v)))
            (when (pair? (cdr lst))
              (set! res (cons lst res))))))
      res)))

;; -------------------------------------------------------------------
;; Simulator: numAircraft CallSigns; per-frame positions via cos/sin.
;; -------------------------------------------------------------------

(define aircraft-t (struct 'aircraft '(cs pos)))

(define simulator-t (struct 'simulator '(aircraft)))

(define (make-simulator n)
  (let ((v (make-vector n #f)))
    (let loop ((i 0))
      (when (< i n)
        (vector-set! v i (cs-t (string-append "foo" (number->string i))))
        (loop (+ i 1))))
    (simulator-t v)))

(define (simulator-simulate sim time)
  (let* ((acft (ref sim 'aircraft))
         (n    (vector-length acft))
         (ct   (cos time))
         (st   (sin time)))
    (let loop ((i 0) (acc '()))
      (cond
        ((>= i n) (reverse acc))
        (else
         (let* ((cs1 (vector-ref acft i))
                (cs2 (vector-ref acft (+ i 1)))
                (p1 (vec3-t time (+ (* ct 2) (* i 3)) 10))
                (p2 (vec3-t time (+ (* st 2) (* i 3)) 10))
                (a1 (aircraft-t cs1 p1))
                (a2 (aircraft-t cs2 p2)))
           (loop (+ i 2) (cons a2 (cons a1 acc)))))))))

;; -------------------------------------------------------------------
;; CollisionDetector
;; -------------------------------------------------------------------

(define detector-t (struct 'detector '(state)))

(define (make-detector) (detector-t (make-rbtree cs-compare)))

(define (handle-new-frame det frame)
  (let* ((state (ref det 'state))
         (seen  (make-rbtree cs-compare))
         (motions
           (let loop ((xs frame) (acc '()))
             (cond
               ((null? xs) (reverse acc))
               (else
                (let* ((ac (car xs))
                       (cs (ref ac 'cs))
                       (newpos (ref ac 'pos))
                       (oldpos (tree-put! state cs newpos)))
                  (tree-put! seen cs #t)
                  (let ((op (if oldpos oldpos newpos)))
                    (loop (cdr xs) (cons (motion-t cs op newpos) acc)))))))))
    ;; remove aircraft not seen this frame
    (let ((to-remove '()))
      (tree-for-each state
        (lambda (cs pos)
          (when (not (tree-get seen cs))
            (set! to-remove (cons cs to-remove)))))
      (for-each (lambda (cs) (tree-remove! state cs)) to-remove))
    ;; pairwise within each voxel group
    (let ((all-reduced (reduce-collision-set motions))
          (count 0))
      (for-each
        (lambda (reduced)
          (let outer ((xs reduced))
            (when (pair? xs)
              (let ((m1 (car xs)))
                (let inner ((ys (cdr xs)))
                  (when (pair? ys)
                    (when (motion-find-intersection m1 (car ys))
                      (set! count (+ count 1)))
                    (inner (cdr ys))))
                (outer (cdr xs))))))
        all-reduced)
      count)))

;; -------------------------------------------------------------------
;; Driver
;; -------------------------------------------------------------------

(define (run-bench)
  (let* ((sim (make-simulator NUM-AIRCRAFT))
         (det (make-detector))
         (total 0))
    (let loop ((i 0))
      (cond
        ((>= i NUM-FRAMES) total)
        (else
         (let* ((time  (/ i 10))
                (frame (simulator-simulate sim time))
                (cnt   (handle-new-frame det frame)))
           (set! total (+ total cnt))
           (loop (+ i 1))))))))

(define (run-iter)
  (let ((c (run-bench)))
    (cond
      ((= c EXPECTED-COLLISIONS) #t)
      (else
       (display "cdjs: bad collision count: ")
       (display c)
       (display " (expected ")
       (display EXPECTED-COLLISIONS)
       (display ")") (newline)
       (exit 1)))))

(define (run-suite n)
  (cond
    ((<= n 0) #t)
    (else (run-iter) (run-suite (- n 1)))))

(run-suite ITERS)
(displayn "ok")
