;; SPDX-License-Identifier: GPL-2.0-or-later
;; Copyright 2008 the V8 project authors. All rights reserved.
;; Copyright 1996 John Maloney and Mario Wolczko.
;; This is a translated and modified version of the DeltaBlue benchmark.

;; deltablue: incremental constraint solver. Builds an equality chain plus
;; a network of scale/offset projections, repeatedly edits one endpoint and
;; propagates. Stresses dispatch, struct + list mutation, and worklist loops.
;;
;; Derived from the SOM benchmarks (Are We Fast Yet?, Marr et al. 2016).

(define CHAIN-N      12000)
(define PROJECTION-N 12000)

;; --- Strengths (just integers; smaller = stronger) ---------------------
(define ABSOLUTE-STRONGEST -10000)
(define REQUIRED             -800)
(define STRONG-PREFERRED     -600)
(define PREFERRED            -400)
(define STRONG-DEFAULT       -200)
(define DEFAULT-STRENGTH        0)
(define WEAK-DEFAULT          500)
(define ABSOLUTE-WEAKEST    10000)

(define (str-stronger? a b) (< a b))
(define (str-weaker?   a b) (> a b))
(define (str-weakest   a b) (if (> a b) a b))

;; --- Variable ---------------------------------------------------------
(define var-t (struct 'var '(value constraints determined-by walk-strength stay mark)))

(define (make-var initial)
  (var-t initial '() '() ABSOLUTE-WEAKEST #t 0))

(define (list-without lst x)
  (cond ((null? lst) '())
        ((eq? (car lst) x) (cdr lst))
        (else (cons (car lst) (list-without (cdr lst) x)))))

(define (var-add-c! v c)
  (setf! v 'constraints (cons c (ref v 'constraints))))

(define (var-remove-c! v c)
  (setf! v 'constraints (list-without (ref v 'constraints) c))
  (if (eq? (ref v 'determined-by) c)
      (setf! v 'determined-by '())))

;; --- Constraint -------------------------------------------------------
;;   kind      kind tag
;;   strength  strength (int)
;;   v1        unary output / binary v1
;;   v2        binary v2 (#f for unary)
;;   dir       binary: DIR-NONE / DIR-FORWARD / DIR-BACKWARD
;;             unary:  satisfied flag (#t / #f)
;;   scale     ScaleConstraint: scale variable
;;   offset    ScaleConstraint: offset variable
(define constraint-t (struct 'constraint '(kind strength v1 v2 dir scale offset)))

(define K-STAY     0)
(define K-EDIT     1)
(define K-EQUALITY 2)
(define K-SCALE    3)

(define DIR-NONE     0)
(define DIR-FORWARD  1)
(define DIR-BACKWARD 2)

(define (c-unary? c)
  (let ((k (ref c 'kind)))
    (or (= k K-STAY) (= k K-EDIT))))

(define (c-input? c) (= (ref c 'kind) K-EDIT))

(define (c-satisfied? c)
  (if (c-unary? c)
      (ref c 'dir)
      (not (= (ref c 'dir) DIR-NONE))))

(define (c-mark-unsatisfied! c)
  (if (c-unary? c)
      (setf! c 'dir #f)
      (setf! c 'dir DIR-NONE)))

(define (c-output c)
  (if (c-unary? c)
      (ref c 'v1)
      (if (= (ref c 'dir) DIR-FORWARD)
          (ref c 'v2)
          (ref c 'v1))))

(define (c-add-to-graph! c)
  (let ((k (ref c 'kind)))
    (cond
      ((or (= k K-STAY) (= k K-EDIT))
       (var-add-c! (ref c 'v1) c)
       (setf! c 'dir #f))
      ((= k K-EQUALITY)
       (var-add-c! (ref c 'v1) c)
       (var-add-c! (ref c 'v2) c)
       (setf! c 'dir DIR-NONE))
      ((= k K-SCALE)
       (var-add-c! (ref c 'v1) c)
       (var-add-c! (ref c 'v2) c)
       (var-add-c! (ref c 'scale) c)
       (var-add-c! (ref c 'offset) c)
       (setf! c 'dir DIR-NONE)))))

(define (c-remove-from-graph! c)
  (let ((k (ref c 'kind)))
    (cond
      ((or (= k K-STAY) (= k K-EDIT))
       (var-remove-c! (ref c 'v1) c)
       (setf! c 'dir #f))
      ((= k K-EQUALITY)
       (var-remove-c! (ref c 'v1) c)
       (var-remove-c! (ref c 'v2) c)
       (setf! c 'dir DIR-NONE))
      ((= k K-SCALE)
       (var-remove-c! (ref c 'v1) c)
       (var-remove-c! (ref c 'v2) c)
       (var-remove-c! (ref c 'scale) c)
       (var-remove-c! (ref c 'offset) c)
       (setf! c 'dir DIR-NONE)))))

(define (c-choose-method! c mark)
  (let ((k (ref c 'kind)))
    (cond
      ((or (= k K-STAY) (= k K-EDIT))
       (let ((out (ref c 'v1)))
         (setf! c 'dir
               (and (not (= (ref out 'mark) mark))
                    (str-stronger? (ref c 'strength) (ref out 'walk-strength))))))
      (else
       (let ((v1 (ref c 'v1)) (v2 (ref c 'v2)) (s (ref c 'strength)))
         (cond
           ((= (ref v1 'mark) mark)
            (if (and (not (= (ref v2 'mark) mark))
                     (str-stronger? s (ref v2 'walk-strength)))
                (setf! c 'dir DIR-FORWARD)
                (setf! c 'dir DIR-NONE)))
           ((= (ref v2 'mark) mark)
            (if (and (not (= (ref v1 'mark) mark))
                     (str-stronger? s (ref v1 'walk-strength)))
                (setf! c 'dir DIR-BACKWARD)
                (setf! c 'dir DIR-NONE)))
           ((str-weaker? (ref v1 'walk-strength) (ref v2 'walk-strength))
            (if (str-stronger? s (ref v1 'walk-strength))
                (setf! c 'dir DIR-BACKWARD)
                (setf! c 'dir DIR-NONE)))
           (else
            (if (str-stronger? s (ref v2 'walk-strength))
                (setf! c 'dir DIR-FORWARD)
                (setf! c 'dir DIR-NONE)))))))))

(define (c-inputs-do c fn)
  (let ((k (ref c 'kind)))
    (cond
      ((or (= k K-STAY) (= k K-EDIT)) 0)
      ((= k K-EQUALITY)
       (if (= (ref c 'dir) DIR-FORWARD)
           (fn (ref c 'v1))
           (fn (ref c 'v2))))
      ((= k K-SCALE)
       (if (= (ref c 'dir) DIR-FORWARD)
           (begin (fn (ref c 'v1)) (fn (ref c 'scale)) (fn (ref c 'offset)))
           (begin (fn (ref c 'v2)) (fn (ref c 'scale)) (fn (ref c 'offset))))))))

(define (input-known? v mark)
  (or (= (ref v 'mark) mark)
      (ref v 'stay)
      (null? (ref v 'determined-by))))

(define (c-inputs-known? c mark)
  (let ((k (ref c 'kind)))
    (cond
      ((or (= k K-STAY) (= k K-EDIT)) #t)
      ((= k K-EQUALITY)
       (input-known? (if (= (ref c 'dir) DIR-FORWARD) (ref c 'v1) (ref c 'v2))
                     mark))
      ((= k K-SCALE)
       (let ((primary (if (= (ref c 'dir) DIR-FORWARD) (ref c 'v1) (ref c 'v2))))
         (and (input-known? primary mark)
              (input-known? (ref c 'scale) mark)
              (input-known? (ref c 'offset) mark)))))))

(define (c-execute! c)
  (let ((k (ref c 'kind)))
    (cond
      ((or (= k K-STAY) (= k K-EDIT)) 0)
      ((= k K-EQUALITY)
       (if (= (ref c 'dir) DIR-FORWARD)
           (setf! (ref c 'v2) 'value (ref (ref c 'v1) 'value))
           (setf! (ref c 'v1) 'value (ref (ref c 'v2) 'value))))
      ((= k K-SCALE)
       (let ((scale (ref (ref c 'scale) 'value))
             (offset (ref (ref c 'offset) 'value)))
         (if (= (ref c 'dir) DIR-FORWARD)
             (setf! (ref c 'v2) 'value
                   (+ (* (ref (ref c 'v1) 'value) scale) offset))
             (setf! (ref c 'v1) 'value
                   (/ (- (ref (ref c 'v2) 'value) offset) scale))))))))

(define (c-recalculate! c)
  (let ((k (ref c 'kind)))
    (cond
      ((or (= k K-STAY) (= k K-EDIT))
       (let ((out (ref c 'v1)))
         (setf! out 'walk-strength (ref c 'strength))
         (setf! out 'stay (not (c-input? c)))
         (if (ref out 'stay) (c-execute! c))))
      ((= k K-EQUALITY)
       (let ((fwd (= (ref c 'dir) DIR-FORWARD)))
         (let ((in (if fwd (ref c 'v1) (ref c 'v2)))
               (out (if fwd (ref c 'v2) (ref c 'v1))))
           (setf! out 'walk-strength
                 (str-weakest (ref c 'strength) (ref in 'walk-strength)))
           (setf! out 'stay (ref in 'stay))
           (if (ref out 'stay) (c-execute! c)))))
      ((= k K-SCALE)
       (let ((fwd (= (ref c 'dir) DIR-FORWARD)))
         (let ((in (if fwd (ref c 'v1) (ref c 'v2)))
               (out (if fwd (ref c 'v2) (ref c 'v1))))
           (setf! out 'walk-strength
                 (str-weakest (ref c 'strength) (ref in 'walk-strength)))
           (setf! out 'stay
                 (and (ref in 'stay)
                      (ref (ref c 'scale) 'stay)
                      (ref (ref c 'offset) 'stay)))
           (if (ref out 'stay) (c-execute! c))))))))

;; --- Constraint constructors ------------------------------------------
(define (make-constraint kind strength)
  (constraint-t kind strength #f #f #f #f #f))

(define (planner-add-constraint! planner c)
  (c-add-to-graph! c)
  (planner-incremental-add! planner c))

(define (new-stay! v strength planner)
  (let ((c (make-constraint K-STAY strength)))
    (setf! c 'v1 v)
    (setf! c 'dir #f)
    (planner-add-constraint! planner c)
    c))

(define (new-edit! v strength planner)
  (let ((c (make-constraint K-EDIT strength)))
    (setf! c 'v1 v)
    (setf! c 'dir #f)
    (planner-add-constraint! planner c)
    c))

(define (new-equality! v1 v2 strength planner)
  (let ((c (make-constraint K-EQUALITY strength)))
    (setf! c 'v1 v1)
    (setf! c 'v2 v2)
    (setf! c 'dir DIR-NONE)
    (planner-add-constraint! planner c)
    c))

(define (new-scale! src scale offset dst strength planner)
  (let ((c (make-constraint K-SCALE strength)))
    (setf! c 'v1 src)
    (setf! c 'v2 dst)
    (setf! c 'dir DIR-NONE)
    (setf! c 'scale scale)
    (setf! c 'offset offset)
    (planner-add-constraint! planner c)
    c))

(define (destroy-constraint! c planner)
  (if (c-satisfied? c)
      (planner-incremental-remove! planner c))
  (c-remove-from-graph! c))

;; --- Planner ----------------------------------------------------------
(define planner-t (struct 'planner '(mark)))

(define (make-planner) (planner-t 1))

(define (planner-new-mark! p)
  (let ((m (ref p 'mark)))
    (setf! p 'mark (+ m 1))
    m))

(define (planner-incremental-add! planner c)
  (let ((mark (planner-new-mark! planner)))
    (let loop ((overridden (constraint-satisfy! c mark planner)))
      (if (null? overridden)
          0
          (loop (constraint-satisfy! overridden mark planner))))))

(define (constraint-satisfy! c mark planner)
  (c-choose-method! c mark)
  (cond
    ((c-satisfied? c)
     (c-inputs-do c (lambda (i) (setf! i 'mark mark)))
     (let ((out (c-output c)))
       (let ((overridden (ref out 'determined-by)))
         (if (not (null? overridden))
             (c-mark-unsatisfied! overridden))
         (setf! out 'determined-by c)
         (planner-add-propagate! planner c mark)
         (setf! out 'mark mark)
         (if (null? overridden) '() overridden))))
    (else
     (if (= (ref c 'strength) REQUIRED)
         (begin (display "failed to satisfy required\n") (exit 1)))
     '())))

(define (add-cct-to v determining-c todo)
  (let loop ((cs (ref v 'constraints)) (out todo))
    (if (null? cs)
        out
        (let ((c (car cs)))
          (if (and (not (eq? c determining-c)) (c-satisfied? c))
              (loop (cdr cs) (cons c out))
              (loop (cdr cs) out))))))

(define (add-cct-out v todo)
  (add-cct-to v (ref v 'determined-by) todo))

(define (planner-add-propagate! planner c mark)
  (let loop ((todo (list c)))
    (if (null? todo)
        #t
        (let ((d (car todo)) (rest (cdr todo)))
          (let ((out (c-output d)))
            (if (= (ref out 'mark) mark)
                (begin
                  (planner-incremental-remove! planner c)
                  #f)
                (begin
                  (c-recalculate! d)
                  (loop (add-cct-out out rest)))))))))

(define (planner-incremental-remove! planner c)
  (let ((out (c-output c)))
    (c-mark-unsatisfied! c)
    (c-remove-from-graph! c)
    (let ((unsatisfied (planner-remove-propagate-from! planner out)))
      (for-each (lambda (u) (planner-incremental-add! planner u)) unsatisfied))))

(define (planner-remove-propagate-from! planner out)
  (setf! out 'determined-by '())
  (setf! out 'walk-strength ABSOLUTE-WEAKEST)
  (setf! out 'stay #t)
  (let loop ((todo (list out)) (unsatisfied '()))
    (if (null? todo)
        (sort-by-strength unsatisfied)
        (let ((v (car todo)) (rest (cdr todo)))
          (let ((un2 (collect-unsatisfied (ref v 'constraints) unsatisfied))
                (det (ref v 'determined-by)))
            (let ((todo2
                   (let walk ((cs (ref v 'constraints)) (acc rest))
                     (if (null? cs)
                         acc
                         (let ((c (car cs)))
                           (if (and (not (eq? c det)) (c-satisfied? c))
                               (begin (c-recalculate! c)
                                      (walk (cdr cs) (cons (c-output c) acc)))
                               (walk (cdr cs) acc)))))))
              (loop todo2 un2)))))))

(define (collect-unsatisfied cs acc)
  (if (null? cs)
      acc
      (let ((c (car cs)))
        (if (c-satisfied? c)
            (collect-unsatisfied (cdr cs) acc)
            (collect-unsatisfied (cdr cs) (cons c acc))))))

;; insertion sort: stronger (smaller strength int) first
(define (sort-by-strength xs)
  (if (null? xs)
      '()
      (insert-sorted (car xs) (sort-by-strength (cdr xs)))))

(define (insert-sorted x sorted)
  (if (null? sorted)
      (list x)
      (if (str-stronger? (ref x 'strength) (ref (car sorted) 'strength))
          (cons x sorted)
          (cons (car sorted) (insert-sorted x (cdr sorted))))))

(define (planner-extract-plan planner sources)
  (let ((mark (planner-new-mark! planner)))
    (let loop ((todo sources) (plan '()))
      (if (null? todo)
          (reverse plan)
          (let ((c (car todo)) (rest (cdr todo)))
            (let ((out (c-output c)))
              (if (and (not (= (ref out 'mark) mark))
                       (c-inputs-known? c mark))
                  (begin
                    (setf! out 'mark mark)
                    (loop (add-cct-out out rest) (cons c plan)))
                  (loop rest plan))))))))

(define (extract-plan-from-c planner c)
  ;; sources = the single c if input + satisfied, else empty
  (planner-extract-plan
   planner
   (if (and (c-input? c) (c-satisfied? c)) (list c) '())))

(define (plan-execute! plan)
  (for-each (lambda (c) (c-execute! c)) plan))

(define (planner-change-var! planner var val)
  (let ((edit (new-edit! var PREFERRED planner)))
    (let ((plan (extract-plan-from-c planner edit)))
      (let loop ((i 0))
        (if (< i 10)
            (begin
              (setf! var 'value val)
              (plan-execute! plan)
              (loop (+ i 1)))))
      (destroy-constraint! edit planner))))

;; --- Tests -------------------------------------------------------------
(define (assert! ok tag)
  (if (not ok)
      (begin (display "deltablue: ") (display tag) (display " failed\n") (exit 1))))

(define (chain-test n)
  (let ((planner (make-planner))
        (vars (make-vector (+ n 1) 0)))
    (let loop ((i 0))
      (if (<= i n)
          (begin (setf! vars i (make-var 0)) (loop (+ i 1)))))
    (let loop ((i 0))
      (if (< i n)
          (begin
            (new-equality! (ref vars i) (ref vars (+ i 1)) REQUIRED planner)
            (loop (+ i 1)))))
    (new-stay! (ref vars n) STRONG-DEFAULT planner)
    (let ((edit (new-edit! (ref vars 0) PREFERRED planner)))
      (let ((plan (extract-plan-from-c planner edit)))
        (let loop ((v 1))
          (if (<= v 100)
              (begin
                (setf! (ref vars 0) 'value v)
                (plan-execute! plan)
                (assert! (= (ref (ref vars n) 'value) v) "chain")
                (loop (+ v 1))))))
      (destroy-constraint! edit planner))))

(define (projection-test n)
  (let ((planner (make-planner))
        (scale (make-var 10))
        (offset (make-var 1000))
        (dests (make-vector n 0)))
    (let loop ((i 0) (last-src '()) (last-dst '()))
      (if (= i n)
          (begin
            (planner-change-var! planner last-src 17)
            (assert! (= (ref last-dst 'value) 1170) "projection-1")
            (planner-change-var! planner last-dst 1050)
            (assert! (= (ref last-src 'value) 5) "projection-2")
            (planner-change-var! planner scale 5)
            (let lp ((j 0))
              (if (< j (- n 1))
                  (begin
                    (assert! (= (ref (ref dests j) 'value) (+ (* (+ j 1) 5) 1000))
                             "projection-3")
                    (lp (+ j 1)))))
            (planner-change-var! planner offset 2000)
            (let lp ((j 0))
              (if (< j (- n 1))
                  (begin
                    (assert! (= (ref (ref dests j) 'value) (+ (* (+ j 1) 5) 2000))
                             "projection-4")
                    (lp (+ j 1))))))
          (let ((src (make-var (+ i 1)))
                (dst (make-var (+ i 1))))
            (setf! dests i dst)
            (new-stay! src DEFAULT-STRENGTH planner)
            (new-scale! src scale offset dst REQUIRED planner)
            (loop (+ i 1) src dst))))))

(chain-test CHAIN-N)
(projection-test PROJECTION-N)
(displayn "ok")
