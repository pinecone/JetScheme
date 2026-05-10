;; nbody: 3D gravitational N-body simulation. 5 bodies (sun + 4 outer
;; planets), Euler integration. Stresses float math (mul/div/sqrt) and
;; struct field mutation.

(define ITERS 250000)
(define EXPECTED -0.1690859889909308)

(define body-t (struct 'body '(x y z vx vy vz mass)))

(define PI            3.141592653589793)
(define SOLAR-MASS    (* 4.0 PI PI))
(define DAYS-PER-YEAR 365.24)

(define (make-body x y z vx vy vz mass)
  (body-t x y z
          (* vx DAYS-PER-YEAR)
          (* vy DAYS-PER-YEAR)
          (* vz DAYS-PER-YEAR)
          (* mass SOLAR-MASS)))

(define (sun)
  (make-body 0.0 0.0 0.0 0.0 0.0 0.0 1.0))

(define (jupiter)
  (make-body  4.84143144246472090e+00
             -1.16032004402742839e+00
             -1.03622044471123109e-01
              1.66007664274403694e-03
              7.69901118419740425e-03
             -6.90460016972063023e-05
              9.54791938424326609e-04))

(define (saturn)
  (make-body  8.34336671824457987e+00
              4.12479856412430479e+00
             -4.03523417114321381e-01
             -2.76742510726862411e-03
              4.99852801234917238e-03
              2.30417297573763929e-05
              2.85885980666130812e-04))

(define (uranus)
  (make-body  1.28943695621391310e+01
             -1.51111514016986312e+01
             -2.23307578892655734e-01
              2.96460137564761618e-03
              2.37847173959480950e-03
             -2.96589568540237556e-05
              4.36624404335156298e-05))

(define (neptune)
  (make-body  1.53796971148509165e+01
             -2.59193146099879641e+01
              1.79258772950371181e-01
              2.68067772490389322e-03
              1.62824170038242295e-03
             -9.51592254519715870e-05
              5.15138902046611451e-05))

(define BODIES   (vector (sun) (jupiter) (saturn) (uranus) (neptune)))
(define NBODIES  (vector-length BODIES))

;; Adjust the sun's velocity so total momentum is zero.
(define (offset-momentum!)
  (let loop ((i 0) (px 0.0) (py 0.0) (pz 0.0))
    (if (= i NBODIES)
        (let ((s (ref BODIES 0)))
          (setf! (ref s 'vx) (- (/ px SOLAR-MASS)))
          (setf! (ref s 'vy) (- (/ py SOLAR-MASS)))
          (setf! (ref s 'vz) (- (/ pz SOLAR-MASS))))
        (let* ((b (ref BODIES i))
               (m (ref b 'mass)))
          (loop (+ i 1)
                (+ px (* (ref b 'vx) m))
                (+ py (* (ref b 'vy) m))
                (+ pz (* (ref b 'vz) m)))))))

(define (advance dt)
  (let i-loop ((i 0))
    (if (< i NBODIES)
        (let ((bi (ref BODIES i)))
          (let j-loop ((j (+ i 1)))
            (if (< j NBODIES)
                (let* ((bj (ref BODIES j))
                       (dx (- (ref bi 'x) (ref bj 'x)))
                       (dy (- (ref bi 'y) (ref bj 'y)))
                       (dz (- (ref bi 'z) (ref bj 'z)))
                       (d2 (+ (* dx dx) (* dy dy) (* dz dz)))
                       (dist (sqrt d2))
                       (mag (/ dt (* d2 dist)))
                       (mi (ref bi 'mass))
                       (mj (ref bj 'mass)))
                  (setf! (ref bi 'vx) (- (ref bi 'vx) (* dx mj mag)))
                  (setf! (ref bi 'vy) (- (ref bi 'vy) (* dy mj mag)))
                  (setf! (ref bi 'vz) (- (ref bi 'vz) (* dz mj mag)))
                  (setf! (ref bj 'vx) (+ (ref bj 'vx) (* dx mi mag)))
                  (setf! (ref bj 'vy) (+ (ref bj 'vy) (* dy mi mag)))
                  (setf! (ref bj 'vz) (+ (ref bj 'vz) (* dz mi mag)))
                  (j-loop (+ j 1)))))
          (i-loop (+ i 1)))))
  (let pos-loop ((i 0))
    (if (< i NBODIES)
        (let ((b (ref BODIES i)))
          (setf! (ref b 'x) (+ (ref b 'x) (* dt (ref b 'vx))))
          (setf! (ref b 'y) (+ (ref b 'y) (* dt (ref b 'vy))))
          (setf! (ref b 'z) (+ (ref b 'z) (* dt (ref b 'vz))))
          (pos-loop (+ i 1))))))

(define (energy)
  (let i-loop ((i 0) (e 0.0))
    (if (= i NBODIES)
        e
        (let* ((bi (ref BODIES i))
               (mi (ref bi 'mass))
               (vx (ref bi 'vx))
               (vy (ref bi 'vy))
               (vz (ref bi 'vz))
               (e  (+ e (* 0.5 mi (+ (* vx vx) (* vy vy) (* vz vz))))))
          (let j-loop ((j (+ i 1)) (e e))
            (if (= j NBODIES)
                (i-loop (+ i 1) e)
                (let* ((bj (ref BODIES j))
                       (dx (- (ref bi 'x) (ref bj 'x)))
                       (dy (- (ref bi 'y) (ref bj 'y)))
                       (dz (- (ref bi 'z) (ref bj 'z)))
                       (d  (sqrt (+ (* dx dx) (* dy dy) (* dz dz)))))
                  (j-loop (+ j 1) (- e (/ (* mi (ref bj 'mass)) d))))))))))

(offset-momentum!)
(let loop ((i 0))
  (if (< i ITERS)
      (begin (advance 0.01) (loop (+ i 1)))))

(let ((e (energy)))
  (if (< (abs (- e EXPECTED)) 1e-13)
      (displayn "ok")
      (begin (display "bad energy: ") (display e) (newline) (exit 1))))
