;; run-tests passes no script arguments
($check (vector? argv))
($check (= 0 (vector-length argv)))
