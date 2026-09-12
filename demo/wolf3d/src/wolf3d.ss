(define (print-help)
  (display "Usage: demo/wolf3d/run [OPTIONS]

Start the game, record a demo, or play a recorded demo.

  --help                 Print this help and exit without loading game data.
  --datadir PATH         Game data directory (default: data_shareware beside run).
  --plus                 Enable enhanced graphics and movement.
  --record-demo FILE     Record level 1 without sound; Escape saves and exits.
  --play-demo FILE       Play a recording without sound, then exit.
  --headless             Play without a window; requires --play-demo.
  --turn-speed NUMBER    Plus turn speed (default: 62.5).
  --walk-speed NUMBER    Plus walk speed (default: 52.5).

Movement speeds must be greater than 0 and at most 100, require --plus,
and cannot be changed during playback. Playback uses the recorded Plus mode.
Choose only one of --record-demo and --play-demo. File paths are relative
to the current directory. An existing recording at FILE is replaced.

Examples:
  demo/wolf3d/run --plus
  demo/wolf3d/run --plus --record-demo wolf.demo
  demo/wolf3d/run --play-demo wolf.demo
  demo/wolf3d/run --play-demo wolf.demo --headless
  demo/wolf3d/run --datadir /path/to/data --plus
"))

(define (parse-options arguments)
  (define (fail message)
    (error (string-append "wolf3d: " message)))
  (define (legacy? option)
    (or (let find ((names '("nomouse" "-nomouse" "/nomouse" "nojoys" "-nojoys" "/nojoys"
                           "nomain" "-nomain" "/nomain" "noems" "-noems" "/noems"
                           "noxms" "-noxms" "/noxms")))
          (and (pair? names) (or (string=? option (car names)) (find (cdr names)))))
        (let skip ((index 0))
          (and (< index (string-length option))
               (if (char-alphabetic? (string-ref option index))
                   (string=? (ascii-downcase (substring option index (string-length option))) "goobers")
                   (skip (+ index 1)))))))
  (define (scan index options)
    (if (= index (vector-length arguments))
        options
        (let* ((name (ref arguments index))
               (entry (assoc name '(("--datadir" . path) ("--record-demo" . file)
                                    ("--play-demo" . file) ("--turn-speed" . number)
                                    ("--walk-speed" . number) ("--plus" . flag)
                                    ("--headless" . flag))))
               (kind (and entry (cdr entry))))
          (cond
            ((string=? name "--help") (print-help) (exit 0))
            ((not kind)
             (unless (legacy? name) (fail (string-append "unknown option: " name)))
             (scan (+ index 1) options))
            ((assoc name options) (fail (string-append name " specified twice")))
            ((eq? kind 'flag) (scan (+ index 1) (cons (cons name #t) options)))
            (else
             (let ((value (and (< (+ index 1) (vector-length arguments))
                               (ref arguments (+ index 1)))))
               (unless (and value (> (string-length value) 0)
                            (not (and (>= (string-length value) 2)
                                      (string=? (substring value 0 2) "--"))))
                 (fail (string-append name " requires a " (symbol->string kind))))
               (if (eq? kind 'number)
                   (let ((number (string->number value)))
                     (unless (and number (> number 0) (<= number 100))
                       (fail (string-append name " requires a number greater than 0 and at most 100")))
                     (scan (+ index 2) (cons (cons name number) options)))
                   (scan (+ index 2) (cons (cons name value) options)))))))))

  (let* ((options (scan 0 '()))
         (playback (assoc "--play-demo" options)))
    (when (and playback (assoc "--record-demo" options))
      (fail "choose one --record-demo or --play-demo file"))
    (when (and (assoc "--headless" options) (not playback))
      (fail "--headless requires --play-demo"))
    (for-each
      (lambda (name)
        (when (assoc name options)
          (when playback (fail (string-append name " cannot change recorded movement")))
          (unless (assoc "--plus" options) (fail (string-append name " requires --plus")))))
      '("--turn-speed" "--walk-speed"))
    options))

(define options (parse-options argv))

(define (option name default)
  (let ((entry (assoc name options)))
    (if entry (cdr entry) default)))

(include "ID_CA.ss")
(include "ID_PM.ss")
(include "WL_DRAW.ss")
(include "ID_VH.ss")
(include "WL_SPRITE.ss")
(include "WL_AGENT.ss")
(include "WL_DEBUG.ss")
(include "WL_ACT1.ss")
(include "WL_ACT2.ss")
(include "ID_SD.ss")
(include "ID_MM.ss")
(include "ID_US_1.ss")
(include "WL_STATE.ss")
(include "ID_IN.ss")
(include "WL_GAME.ss")
(include "WL_INTER.ss")
(include "WL_TEXT.ss")
(include "WL_MENU.ss")
(include "WL_MAIN.ss")
(include "ID_VL.ss")
(include "demo.ss")
(include "plus.ss")

(CheckForEpisodes)
(Patch386)
(InitGame)

(define title-frames 0)
(define title-work 0)
(define title-update (time-monotonic))

(define (update-title frame-start)
  (let ((now (time-monotonic)))
    (set! title-frames (+ title-frames 1))
    (set! title-work (+ title-work (- now frame-start)))
    (when (>= (- now title-update) 1)
      (dos:set-window-title
       (string-append "Wolfenstein 3D - "
                      (number->string (/ (round (/ (* 10 title-frames) title-work)) 10))
                      " fps"))
      (set! title-frames 0)
      (set! title-work 0)
      (set! title-update now))))

(define (run-game)
  (StartCPMusic INTROSONG)
  (PG13)
  (let outer ()
    (set! ingame #f)
    (DemoLoop)
    (set! ingame #t)
    (DrawPlayScreen)
    (if loadedgame
        (begin
          (set! startgame #f)
          (set! loadedgame #f)
          (StartMusic)
          (DrawLevel)
          (set! playstate ex_stillplaying))
        (start-level))
    (let play ()
      (if (game-step)
          (begin
            (StartCPMusic INTROSONG)
            (outer))
          (begin
            (IN_Yield)
            (play))))))

(define game #f)

(define (frame)
  (unless quitting
    (let ((frame-start (time-monotonic)))
      (IN_PollKeyboard)
      (unless demo-session
        (update-clock)
        (SD_Service))
      (coro/next game #f)
      ;; The host presents the current visible linear RAM image.  The explicit
      ;; display page remains for reference copies, but direct reference writes
      ;; (including FizzleFade's per-VBL steps) must be visible immediately.
      (dos:display-framebuffer framebuffer)
      (update-title frame-start))))

(if demo-headless
    (run-demo)
    (begin
      ;; sokol allows one image update per frame, so only the driver displays a framebuffer.
      (set! vl-vbl (lambda (count) (IN_Yield)))
      (set! game
        (let/coro yield ()
          (set! in-yield yield)
          (if demo-session
              (begin
                (run-demo)
                (dos:request-quit))
              (run-game))))
      (dos:frame-loop "Wolfenstein 3D" screenwidth screenheight 'crt frame)))
