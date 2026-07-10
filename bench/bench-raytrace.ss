;; raytrace: 3D ray tracer (Burmister/Octane). Polymorphic shapes and
;; materials, vector math, reflection/shadow/highlight recursion.
;; Stresses float math, allocation, and dispatch. A correct run prints 2321.

(define check-number 0)

;; --- Color -----------------------------------------------------------
(define color-t (struct 'color '(red green blue)))

(define (make-color r g b)
  (color-t (if r r 0.0) (if g g 0.0) (if b b 0.0)))

(define (color-limit-value v)
  (if (> v 1.0) 1.0 (if (< v 0.0) 0.0 v)))

(define (color-limit! c)
  (setf! (ref c 'red) (color-limit-value (ref c 'red)))
  (setf! (ref c 'green) (color-limit-value (ref c 'green)))
  (setf! (ref c 'blue) (color-limit-value (ref c 'blue))))

(define (color-add c1 c2)
  (color-t (+ (ref c1 'red) (ref c2 'red))
           (+ (ref c1 'green) (ref c2 'green))
           (+ (ref c1 'blue) (ref c2 'blue))))

(define (color-add-scalar c1 s)
  (color-t (color-limit-value (+ (ref c1 'red) s))
           (color-limit-value (+ (ref c1 'green) s))
           (color-limit-value (+ (ref c1 'blue) s))))

(define (color-subtract c1 c2)
  (color-t (- (ref c1 'red) (ref c2 'red))
           (- (ref c1 'green) (ref c2 'green))
           (- (ref c1 'blue) (ref c2 'blue))))

(define (color-multiply c1 c2)
  (color-t (* (ref c1 'red) (ref c2 'red))
           (* (ref c1 'green) (ref c2 'green))
           (* (ref c1 'blue) (ref c2 'blue))))

(define (color-multiply-scalar c1 f)
  (color-t (* (ref c1 'red) f)
           (* (ref c1 'green) f)
           (* (ref c1 'blue) f)))

(define (color-divide-factor c1 f)
  (color-t (/ (ref c1 'red) f)
           (/ (ref c1 'green) f)
           (/ (ref c1 'blue) f)))

(define (color-distance c1 c2)
  (+ (+ (abs (- (ref c1 'red) (ref c2 'red)))
        (abs (- (ref c1 'green) (ref c2 'green))))
     (abs (- (ref c1 'blue) (ref c2 'blue)))))

(define (color-blend c1 c2 w)
  (color-add (color-multiply-scalar c1 (- 1.0 w))
             (color-multiply-scalar c2 w)))

(define (color-brightness c)
  (let ((r (floor (* (ref c 'red) 255)))
        (g (floor (* (ref c 'green) 255)))
        (b (floor (* (ref c 'blue) 255))))
    (arithmetic-shift (+ (+ (* r 77) (* g 150)) (* b 29)) -8)))

;; --- Vector ----------------------------------------------------------
(define vector-t (struct 'vector '(x y z)))

(define (make-vector x y z)
  (vector-t (if x x 0.0) (if y y 0.0) (if z z 0.0)))

(define (vector-magnitude v)
  (sqrt (+ (+ (* (ref v 'x) (ref v 'x)) (* (ref v 'y) (ref v 'y))) (* (ref v 'z) (ref v 'z)))))

(define (vector-normalize v)
  (let ((m (vector-magnitude v)))
    (vector-t (/ (ref v 'x) m) (/ (ref v 'y) m) (/ (ref v 'z) m))))

(define (vector-cross v w)
  (vector-t (+ (* (- (ref v 'z)) (ref w 'y)) (* (ref v 'y) (ref w 'z)))
            (- (* (ref v 'z) (ref w 'x)) (* (ref v 'x) (ref w 'z)))
            (+ (* (- (ref v 'y)) (ref w 'x)) (* (ref v 'x) (ref w 'y)))))

(define (vector-dot v w)
  (+ (+ (* (ref v 'x) (ref w 'x)) (* (ref v 'y) (ref w 'y))) (* (ref v 'z) (ref w 'z))))

(define (vector-add v w)
  (vector-t (+ (ref v 'x) (ref w 'x))
            (+ (ref v 'y) (ref w 'y))
            (+ (ref v 'z) (ref w 'z))))

(define (vector-subtract v w)
  (vector-t (- (ref v 'x) (ref w 'x))
            (- (ref v 'y) (ref w 'y))
            (- (ref v 'z) (ref w 'z))))

(define (vector-multiply-scalar v w)
  (vector-t (* (ref v 'x) w) (* (ref v 'y) w) (* (ref v 'z) w)))

(define (vector-multiply-vector v w)
  (vector-t (* (ref v 'x) (ref w 'x))
            (* (ref v 'y) (ref w 'y))
            (* (ref v 'z) (ref w 'z))))

;; --- Ray, Light, Background ------------------------------------------
(define ray-t (struct 'ray '(position direction)))
(define (make-ray pos dir) (ray-t pos dir))

(define light-t (struct 'light '(position color intensity)))
(define (make-light pos col intensity)
  (light-t pos col (if intensity intensity 10.0)))

(define background-t (struct 'background '(color ambience)))
(define (make-background col amb) (background-t col amb))

;; --- IntersectionInfo ------------------------------------------------
(define intersection-info-t (struct 'intersection-info '(is-hit hit-count shape position normal color distance)))
(define (make-intersection-info)
  (intersection-info-t #f 0 #f #f #f (make-color 0.0 0.0 0.0) #f))

;; --- Materials -------------------------------------------------------
(define solid-t (struct 'solid '(color reflection transparency gloss has-texture)))
(define (make-solid-material col reflection refraction transparency gloss)
  (solid-t col reflection transparency gloss #f))

(define chessboard-t (struct 'chessboard '(color-even color-odd reflection transparency gloss density has-texture)))
(define (make-chessboard-material color-even color-odd reflection transparency gloss density)
  (chessboard-t color-even color-odd reflection transparency gloss density #t))

(define (wrap-up t)
  (let ((t (modulo t 2.0)))
    (let ((t (if (< t -1.0) (+ t 2.0) t)))
      (if (>= t 1.0) (- t 2.0) t))))

(define (material-get-color mat u v)
  (if (isa? mat solid-t)
      (ref mat 'color)
      (let ((t (* (wrap-up (* u (ref mat 'density)))
                  (wrap-up (* v (ref mat 'density))))))
        (if (< t 0.0)
            (ref mat 'color-even)
            (ref mat 'color-odd)))))

;; --- Shapes ----------------------------------------------------------
(define sphere-t (struct 'sphere '(radius position material)))
(define (make-sphere position radius material)
  (sphere-t radius position material))

(define plane-t (struct 'plane '(position d material)))
(define (make-plane position d material)
  (plane-t position d material))

(define (sphere-intersect self ray)
  (let* ((info (make-intersection-info))
         (dst (vector-subtract (ref ray 'position) (ref self 'position)))
         (B (vector-dot dst (ref ray 'direction)))
         (C (- (vector-dot dst dst) (* (ref self 'radius) (ref self 'radius))))
         (D (- (* B B) C)))
    (setf! (ref info 'shape) self)
    (if (> D 0)
        (begin
          (setf! (ref info 'is-hit) #t)
          (setf! (ref info 'distance) (- (- B) (sqrt D)))
          (setf! (ref info 'position)
                 (vector-add (ref ray 'position)
                             (vector-multiply-scalar (ref ray 'direction) (ref info 'distance))))
          (setf! (ref info 'normal)
                 (vector-normalize (vector-subtract (ref info 'position) (ref self 'position))))
          (setf! (ref info 'color) (material-get-color (ref self 'material) 0.0 0.0)))
        (setf! (ref info 'is-hit) #f))
    info))

(define (plane-intersect self ray)
  (let* ((info (make-intersection-info))
         (Vd (vector-dot (ref self 'position) (ref ray 'direction))))
    (if (= Vd 0)
        info
        (let ((t (- (/ (+ (vector-dot (ref self 'position) (ref ray 'position)) (ref self 'd)) Vd))))
          (if (<= t 0)
              info
              (begin
                (setf! (ref info 'shape) self)
                (setf! (ref info 'is-hit) #t)
                (setf! (ref info 'position)
                       (vector-add (ref ray 'position)
                                   (vector-multiply-scalar (ref ray 'direction) t)))
                (setf! (ref info 'normal) (ref self 'position))
                (setf! (ref info 'distance) t)
                (setf! (ref info 'color) (material-get-color (ref self 'material) 0.0 0.0))
                (if (ref (ref self 'material) 'has-texture)
                    (let* ((vU (make-vector (ref (ref self 'position) 'y)
                                            (ref (ref self 'position) 'z)
                                            (- (ref (ref self 'position) 'x))))
                           (vV (vector-cross vU (ref self 'position)))
                           (u (vector-dot (ref info 'position) vU))
                           (v (vector-dot (ref info 'position) vV)))
                      (setf! (ref info 'color)
                             (material-get-color (ref self 'material) u v))))
                info))))))

(define (shape-intersect shape ray)
  (if (isa? shape sphere-t)
      (sphere-intersect shape ray)
      (plane-intersect shape ray)))

;; --- Camera ----------------------------------------------------------
(define camera-t (struct 'camera '(position look-at equator up screen)))
(define (make-camera position look-at up)
  (let ((equator (vector-cross (vector-normalize look-at) up))
        (screen (vector-add position look-at)))
    (camera-t position look-at equator up screen)))

(define (camera-get-ray self vx vy)
  (let* ((pos (vector-subtract
                (ref self 'screen)
                (vector-subtract
                  (vector-multiply-scalar (ref self 'equator) vx)
                  (vector-multiply-scalar (ref self 'up) vy)))))
    (setf! (ref pos 'y) (* -1 (ref pos 'y)))
    (let ((dir (vector-subtract pos (ref self 'position))))
      (make-ray pos (vector-normalize dir)))))

;; --- Scene -----------------------------------------------------------
(define scene-t (struct 'scene '(camera shapes lights background)))
(define (make-scene)
  (scene-t (make-camera (make-vector 0.0 0.0 -5.0)
                        (make-vector 0.0 0.0 1.0)
                        (make-vector 0.0 1.0 0.0))
           (vector)
           (vector)
           (make-background (make-color 0.0 0.0 0.5) 0.2)))

;; --- Engine ----------------------------------------------------------
(define engine-t (struct 'engine '(canvas-height canvas-width pixel-width pixel-height render-diffuse render-shadows render-highlights render-reflections ray-depth canvas check-number)))
(define (make-engine canvas-width canvas-height pixel-width pixel-height render-diffuse render-shadows render-highlights render-reflections ray-depth)
  (engine-t (/ canvas-height pixel-height)
            (/ canvas-width pixel-width)
            pixel-width
            pixel-height
            render-diffuse
            render-shadows
            render-highlights
            render-reflections
            ray-depth
            #f
            0))

(define (engine-set-pixel self x y col)
  (if (ref self 'canvas)
      #f
      (if (= x y)
          (setf! (ref self 'check-number) (+ (ref self 'check-number) (color-brightness col)))
          #f)))

(define (engine-test-intersection self ray scene exclude)
  (let loop ((i 0) (hits 0) (best #f) (best-distance 2000.0))
    (if (>= i (vector-length (ref scene 'shapes)))
        (let ((best (if best best (make-intersection-info))))
          (setf! (ref best 'distance) best-distance)
          (setf! (ref best 'hit-count) hits)
          best)
        (let* ((shape (vector-ref (ref scene 'shapes) i)))
          (if (eq? shape exclude)
              (loop (+ i 1) hits best best-distance)
              (let ((info (shape-intersect shape ray)))
                (if (and (ref info 'is-hit)
                         (>= (ref info 'distance) 0)
                         (< (ref info 'distance) best-distance))
                    (loop (+ i 1) (+ hits 1) info (ref info 'distance))
                    (loop (+ i 1) hits best best-distance))))))))

(define (engine-get-reflection-ray self P N V)
  (let* ((c1 (- (vector-dot N V)))
         (R1 (vector-add (vector-multiply-scalar N (* 2.0 c1)) V)))
    (make-ray P R1)))

(define (engine-ray-trace self info ray scene depth)
  (let ((col (color-multiply-scalar (ref info 'color) (ref (ref scene 'background) 'ambience)))
        (shininess (expt 10 (+ (ref (ref (ref info 'shape) 'material) 'gloss) 1))))
    (let light-loop ((i 0))
      (if (>= i (vector-length (ref scene 'lights)))
          (begin (color-limit! col) col)
          (let* ((light (vector-ref (ref scene 'lights) i))
                 (v (vector-normalize (vector-subtract (ref light 'position) (ref info 'position)))))
            (if (ref self 'render-diffuse)
                (let ((L (vector-dot v (ref info 'normal))))
                  (if (> L 0.0)
                      (set! col (color-add col
                                           (color-multiply (ref info 'color)
                                                           (color-multiply-scalar (ref light 'color) L))))
                      #f))
                #f)
            (if (and (<= depth (ref self 'ray-depth))
                     (ref self 'render-reflections)
                     (> (ref (ref (ref info 'shape) 'material) 'reflection) 0))
                (let* ((reflection-ray (engine-get-reflection-ray self (ref info 'position) (ref info 'normal) (ref ray 'direction)))
                       (refl (engine-test-intersection self reflection-ray scene (ref info 'shape))))
                  (if (and (ref refl 'is-hit) (> (ref refl 'distance) 0))
                      (setf! (ref refl 'color)
                             (engine-ray-trace self refl reflection-ray scene (+ depth 1)))
                      (setf! (ref refl 'color) (ref (ref scene 'background) 'color)))
                  (set! col (color-blend col (ref refl 'color) (ref (ref (ref info 'shape) 'material) 'reflection))))
                #f)
            (let ((shadow-info (make-intersection-info)))
              (if (ref self 'render-shadows)
                  (let* ((shadow-ray (make-ray (ref info 'position) v))
                         (si (engine-test-intersection self shadow-ray scene (ref info 'shape))))
                    (set! shadow-info si)
                    (if (and (ref shadow-info 'is-hit) (not (eq? (ref shadow-info 'shape) (ref info 'shape))))
                        (let* ((vA (color-multiply-scalar col 0.5))
                               (dB (* 0.5 (expt (ref (ref (ref shadow-info 'shape) 'material) 'transparency) 0.5))))
                          (set! col (color-add-scalar vA dB)))
                        #f))
                  #f)
              (if (and (ref self 'render-highlights)
                       (not (ref shadow-info 'is-hit))
                       (> (ref (ref (ref info 'shape) 'material) 'gloss) 0))
                  (let* ((Lv (vector-normalize (vector-subtract (ref (ref info 'shape) 'position) (ref light 'position))))
                         (E (vector-normalize (vector-subtract (ref (ref scene 'camera) 'position) (ref (ref info 'shape) 'position))))
                         (H (vector-normalize (vector-subtract E Lv)))
                         (d (let ((d (vector-dot (ref info 'normal) H))) (if (< d 0) 0 d)))
                         (gloss-weight (expt d shininess)))
                    (set! col (color-add (color-multiply-scalar (ref light 'color) gloss-weight) col)))
                  #f))
            (light-loop (+ i 1)))))))

(define (engine-get-pixel-color self ray scene)
  (let ((info (engine-test-intersection self ray scene #f)))
    (if (ref info 'is-hit)
        (engine-ray-trace self info ray scene 0)
        (ref (ref scene 'background) 'color))))

(define (engine-render-scene self scene)
  (setf! (ref self 'check-number) 0)
  (setf! (ref self 'canvas) #f)
  (let ((canvas-height (ref self 'canvas-height))
        (canvas-width (ref self 'canvas-width)))
    (let y-loop ((y 0))
      (if (>= y canvas-height)
          #f
          (begin
            (let x-loop ((x 0))
              (if (>= x canvas-width)
                  #f
                  (let* ((yp (- (* (/ (* y 1.0) canvas-height) 2.0) 1))
                         (xp (- (* (/ (* x 1.0) canvas-width) 2.0) 1))
                         (ray (camera-get-ray (ref scene 'camera) xp yp))
                         (col (engine-get-pixel-color self ray scene)))
                    (engine-set-pixel self x y col)
                    (x-loop (+ x 1)))))
            (y-loop (+ y 1)))))))

;; --- Scene setup -----------------------------------------------------
(define (build-scene)
  (let ((scene (make-scene)))
    (setf! (ref scene 'camera)
           (make-camera (make-vector 0.0 0.0 -15.0)
                        (make-vector -0.2 0.0 5.0)
                        (make-vector 0.0 1.0 0.0)))
    (setf! (ref scene 'background)
           (make-background (make-color 0.5 0.5 0.5) 0.4))
    (setf! (ref scene 'shapes)
           (vector (make-plane (vector-normalize (make-vector 0.1 0.9 -0.5)) 1.2
                               (make-chessboard-material (make-color 1.0 1.0 1.0)
                                                         (make-color 0.0 0.0 0.0)
                                                         0.2 0.0 1.0 0.7))
                   (make-sphere (make-vector -1.5 1.5 2.0) 1.5
                                (make-solid-material (make-color 0.0 0.5 0.5)
                                                     0.3 0.0 0.0 2.0))
                   (make-sphere (make-vector 1.0 0.25 1.0) 0.5
                                (make-solid-material (make-color 0.9 0.9 0.9)
                                                     0.1 0.0 0.0 1.5))))
    (setf! (ref scene 'lights)
           (vector (make-light (make-vector 5.0 10.0 -1.0)
                               (make-color 0.8 0.8 0.8)
                               #f)
                   (make-light (make-vector -3.0 5.0 -15.0)
                               (make-color 0.8 0.8 0.8)
                               100)))
    scene))

(define (run-once)
  (let ((scene (build-scene)))
    (let ((rt (make-engine 100 100 5 5 #t #t #t #t 2)))
      (engine-render-scene rt scene)
      (ref rt 'check-number))))

(define N 60)
(let loop ((i 0) (result 0))
  (if (>= i N)
      (displayn result)
      (loop (+ i 1) (run-once))))
