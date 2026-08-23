($check (number? (time-monotonic)))
($check (> (time-monotonic) 0))

(define start (time-monotonic))
(let loop ((i 0)) (when (< i 1000000) (loop (+ i 1))))
(define stop (time-monotonic))

($check (>= stop start))
($check (< (- stop start) 60))
