;; richards: OS kernel scheduling simulation. Five tasks (idler, worker,
;; two handlers, two devices) driven by a packet-passing scheduler.
;; Stresses allocation, struct mutation, and indirect dispatch.
;; A correct run produces queueCount = 2322 and holdCount = 928.

(define ID-IDLE 0)
(define ID-WORKER 1)
(define ID-HANDLER-A 2)
(define ID-HANDLER-B 3)
(define ID-DEVICE-A 4)
(define ID-DEVICE-B 5)
(define NUMBER-OF-IDS 6)

(define KIND-DEVICE 0)
(define KIND-WORK 1)

(define DATA-SIZE 4)
(define COUNT 100000)

(define STATE-RUNNING 0)
(define STATE-RUNNABLE 1)
(define STATE-SUSPENDED 2)
(define STATE-SUSPENDED-RUNNABLE 3)
(define STATE-HELD 4)
(define STATE-NOT-HELD (bitwise-not STATE-HELD))

(define EXPECTED-QUEUE-COUNT 232625)
(define EXPECTED-HOLD-COUNT 93050)

;; ---- Packet: link, id, kind, a1, a2; a2 is its own 4-vector. ----

(define packet-t (struct 'packet '(link id kind a1 a2)))

(define (make-packet link id kind)
  (packet-t link id kind 0 (make-vector 4 0)))

(define (packet-add-to packet queue)
  (setf! packet 'link '())
  (if (null? queue)
      packet
      (begin
        (let loop ((next queue))
          (let ((peek (ref next 'link)))
            (if (null? peek)
                (setf! next 'link packet)
                (loop peek))))
        queue)))

;; ---- TCB: link, id, priority, queue, task, state. ----

(define tcb-t (struct 'tcb '(link id priority queue task state)))

(define (make-tcb link id priority queue task)
  (tcb-t link id priority queue task
         (if (null? queue) STATE-SUSPENDED STATE-SUSPENDED-RUNNABLE)))

(define (tcb-held-or-suspended? t)
  (let ((s (ref t 'state)))
    (or (not (= 0 (bitwise-and s STATE-HELD))) (= s STATE-SUSPENDED))))

;; ---- Tasks: kind, v1, v2. ----

(define task-t (struct 'task '(kind v1 v2)))

(define (make-task kind v1 v2) (task-t kind v1 v2))

(define (make-idle-task v1 count)  (make-task 'idle v1 count))
(define (make-worker-task v1 v2)   (make-task 'worker v1 v2))
(define (make-handler-task)        (make-task 'handler '() '()))
(define (make-device-task)         (make-task 'device '() '()))

;; ---- Scheduler state ----

(define queue-count 0)
(define hold-count 0)
(define blocks (make-vector NUMBER-OF-IDS '()))
(define list-head '())
(define current-tcb '())
(define current-id 0)

(define (add-task id priority queue task)
  (let ((tcb (make-tcb list-head id priority queue task)))
    (set! current-tcb tcb)
    (set! list-head tcb)
    (setf! blocks id tcb)))

(define (add-running-task id priority queue task)
  (add-task id priority queue task)
  (setf! current-tcb 'state STATE-RUNNING))

(define (sched-release id)
  (let ((t (ref blocks id)))
    (if (null? t)
        t
        (begin
          (setf! t 'state (bitwise-and (ref t 'state) STATE-NOT-HELD))
          (if (> (ref t 'priority) (ref current-tcb 'priority))
              t
              current-tcb)))))

(define (sched-hold-current)
  (set! hold-count (+ hold-count 1))
  (setf! current-tcb 'state (bitwise-ior (ref current-tcb 'state) STATE-HELD))
  (ref current-tcb 'link))

(define (sched-suspend-current)
  (setf! current-tcb 'state (bitwise-ior (ref current-tcb 'state) STATE-SUSPENDED))
  current-tcb)

(define (sched-queue packet)
  (let ((t (ref blocks (ref packet 'id))))
    (if (null? t)
        t
        (begin
          (set! queue-count (+ queue-count 1))
          (setf! packet 'link '())
          (setf! packet 'id current-id)
          (tcb-check-priority-add t current-tcb packet)))))

(define (tcb-check-priority-add t task packet)
  (cond
    ((null? (ref t 'queue))
     (setf! t 'queue packet)
     (setf! t 'state (bitwise-ior (ref t 'state) STATE-RUNNABLE))
     (if (> (ref t 'priority) (ref task 'priority)) t task))
    (else
     (setf! t 'queue (packet-add-to packet (ref t 'queue)))
     task)))

(define (tcb-run t)
  (cond
    ((= (ref t 'state) STATE-SUSPENDED-RUNNABLE)
     (let ((packet (ref t 'queue)))
       (setf! t 'queue (ref packet 'link))
       (if (null? (ref t 'queue))
           (setf! t 'state STATE-RUNNING)
           (setf! t 'state STATE-RUNNABLE))
       (task-run (ref t 'task) packet)))
    (else
     (task-run (ref t 'task) '()))))

;; ---- Per-task run dispatch ----

(define (task-run task packet)
  (cond
    ((eq? (ref task 'kind) 'idle)    (idle-task-run task packet))
    ((eq? (ref task 'kind) 'worker)  (worker-task-run task packet))
    ((eq? (ref task 'kind) 'handler) (handler-task-run task packet))
    (else                            (device-task-run task packet))))

(define (idle-task-run t packet)
  (setf! t 'v2 (- (ref t 'v2) 1))
  (cond
    ((= (ref t 'v2) 0) (sched-hold-current))
    ((= 0 (bitwise-and (ref t 'v1) 1))
     (setf! t 'v1 (arithmetic-shift (ref t 'v1) -1))
     (sched-release ID-DEVICE-A))
    (else
     (setf! t 'v1 (bitwise-xor (arithmetic-shift (ref t 'v1) -1) 53256))
     (sched-release ID-DEVICE-B))))

(define (device-task-run t packet)
  (cond
    ((null? packet)
     (if (null? (ref t 'v1))
         (sched-suspend-current)
         (let ((v (ref t 'v1)))
           (setf! t 'v1 '())
           (sched-queue v))))
    (else
     (setf! t 'v1 packet)
     (sched-hold-current))))

(define (worker-task-run t packet)
  (cond
    ((null? packet) (sched-suspend-current))
    (else
     (setf! t 'v1 (if (= (ref t 'v1) ID-HANDLER-A) ID-HANDLER-B ID-HANDLER-A))
     (setf! packet 'id (ref t 'v1))
     (setf! packet 'a1 0)
     (let loop ((i 0))
       (when (< i DATA-SIZE)
         (setf! t 'v2 (+ (ref t 'v2) 1))
         (when (> (ref t 'v2) 26) (setf! t 'v2 1))
         (setf! (ref packet 'a2) i (ref t 'v2))
         (loop (+ i 1))))
     (sched-queue packet))))

(define (handler-task-run t packet)
  (when (not (null? packet))
    (if (= (ref packet 'kind) KIND-WORK)
        (setf! t 'v1 (packet-add-to packet (ref t 'v1)))
        (setf! t 'v2 (packet-add-to packet (ref t 'v2)))))
  (cond
    ((null? (ref t 'v1)) (sched-suspend-current))
    (else
     (let ((count (ref (ref t 'v1) 'a1)))
       (cond
         ((< count DATA-SIZE)
          (cond
            ((null? (ref t 'v2)) (sched-suspend-current))
            (else
             (let ((v (ref t 'v2)))
               (setf! t 'v2 (ref v 'link))
               (setf! v 'a1 (ref (ref (ref t 'v1) 'a2) count))
               (setf! (ref t 'v1) 'a1 (+ count 1))
               (sched-queue v)))))
         (else
          (let ((v (ref t 'v1)))
            (setf! t 'v1 (ref v 'link))
            (sched-queue v))))))))

(define (schedule)
  (set! current-tcb list-head)
  (let loop ()
    (cond
      ((null? current-tcb) 'done)
      (else
       (cond
         ((tcb-held-or-suspended? current-tcb)
          (set! current-tcb (ref current-tcb 'link)))
         (else
          (set! current-id (ref current-tcb 'id))
          (set! current-tcb (tcb-run current-tcb))))
       (loop)))))

;; ---- Build and run ----

(define queue '())

(add-running-task ID-IDLE 0 '() (make-idle-task 1 COUNT))

(set! queue (make-packet '()    ID-WORKER KIND-WORK))
(set! queue (make-packet queue  ID-WORKER KIND-WORK))
(add-task ID-WORKER 1000 queue (make-worker-task ID-HANDLER-A 0))

(set! queue (make-packet '()    ID-DEVICE-A KIND-DEVICE))
(set! queue (make-packet queue  ID-DEVICE-A KIND-DEVICE))
(set! queue (make-packet queue  ID-DEVICE-A KIND-DEVICE))
(add-task ID-HANDLER-A 2000 queue (make-handler-task))

(set! queue (make-packet '()    ID-DEVICE-B KIND-DEVICE))
(set! queue (make-packet queue  ID-DEVICE-B KIND-DEVICE))
(set! queue (make-packet queue  ID-DEVICE-B KIND-DEVICE))
(add-task ID-HANDLER-B 3000 queue (make-handler-task))

(add-task ID-DEVICE-A 4000 '() (make-device-task))
(add-task ID-DEVICE-B 5000 '() (make-device-task))

(schedule)

(if (and (= queue-count EXPECTED-QUEUE-COUNT)
         (= hold-count EXPECTED-HOLD-COUNT))
    (displayn (+ queue-count hold-count))
    (begin
      (display "bad: queue=") (display queue-count)
      (display " hold=") (display hold-count) (newline)
      (exit 1)))
