;; Display and string values
(define black_things '("soot" "licorice" "midnight" "some chalkboards"
                       "tabbed\tthing" "before newline\nafter newline"
                       "\"quoted thing\"" "slish\\slash"))

($check (eqv? "soot" (first black_things)))
($check (eqv? "licorice" (second black_things)))
($check (eqv? "midnight" (third black_things)))
($check (eqv? "some chalkboards" (fourth black_things)))
($check (eqv? "tabbed\tthing" (fifth black_things)))
($check (eqv? "slish\\slash" (last black_things)))
