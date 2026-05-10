;; Test that exit actually terminates.
;; If exit works, the $check below is never reached.
(exit 0)
($check #f)
