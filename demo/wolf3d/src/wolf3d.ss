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

(define demo-action
  (cond ((assoc "--record-demo" options) 'record)
        ((assoc "--play-demo" options) 'play)
        (else #f)))
(define demo-path (or (option "--record-demo" #f) (option "--play-demo" #f)))
(define demo-data #f)

(define (demo-error message)
  (error (string-append "demo: " message)))

(when (eq? demo-action 'play)
  (let ((port (open-input-file/maybe demo-path)))
    (unless port (demo-error "cannot open playback file"))
    (set! demo-data (read-bytes/all port))
    (close-input-port port))
  (unless (and (>= (bytevector-length demo-data) 19)
               (= (ref demo-data 0) 87) (= (ref demo-data 1) 79)
               (= (ref demo-data 2) 76) (= (ref demo-data 3) 70)
               (= (ref demo-data 4) 1) (<= (ref demo-data 5) 1)
               (= (ref demo-data 6) 19) (= (ref demo-data 7) gd_hard)
               (= (bytevector-length demo-data) (+ 16 (* 3 (readu32 demo-data 8)))))
    (demo-error "invalid or unsupported demo file"))
  (when (and (option "--plus" #f) (= (ref demo-data 5) 0))
    (demo-error "this recording has --plus disabled"))
  (let controls ((offset 16))
    (when (< offset (bytevector-length demo-data))
      (let ((turn (demo-byte (ref demo-data (+ offset 1))))
            (walk (demo-byte (ref demo-data (+ offset 2)))))
        (when (or (> (abs turn) 100) (> (abs walk) 100))
          (demo-error "movement outside the recorded control range")))
      (controls (+ offset 3)))))

(define demo-frames 0)

(define (demo-checksum)
  (define (mix hash value)
    (modulo (+ (* hash 65599) value) 4294967296))
  (define (bytes buffer hash)
    (let loop ((index 0) (hash hash))
      (if (= index (bytevector-length buffer))
          hash
          (loop (+ index 1) (mix hash (ref buffer index))))))
  (let fields ((values (list player-x player-y player-angle health ammo score rndindex TimeCount
                            killcount treasurecount secretcount))
               (hash (bytes curpal (bytes framebuffer 0))))
    (if (null? values)
        hash
        (let ((text (number->string (car values))))
          (let chars ((index 0) (hash hash))
            (if (= index (string-length text))
                (fields (cdr values) (mix hash 0))
                (chars (+ index 1) (mix hash (char->integer (ref text index))))))))))

(define (demo-put32! buffer offset value)
  (setu16! buffer offset (bitwise-and value 65535))
  (setu16! buffer (+ offset 2) (arithmetic-shift value -16)))

(define (demo-save checksum)
  (when (= demo-frames 0) (demo-error "recording is empty"))
  (let ((header (bytevector 87 79 76 70 1 (if plus-available 1 0) 19 gd_hard 0 0 0 0 0 0 0 0))
        (port (open-output-file demo-path)))
    (demo-put32! header 8 demo-frames)
    (demo-put32! header 12 checksum)
    (write-bytes header port)
    (write-bytes (bytevector-copy demo-buffer 4 demo-pointer) port)
    (close-output-port port)))

(define (run-demo)
  (NewGame gd_hard 0)
  (set! ingame #t)
  (set! mouseenabled (and (eq? demo-action 'record) MousePresent (not demo-headless)))
  (if (eq? demo-action 'record)
      (begin
        (StartDemoRecord 0)
        (display "Recording level 1 without sound. Press Escape to save and exit.\n"))
      (begin
        (set! demo-buffer demo-data)
        (set! demo-pointer 16)
        (set! demo-end (bytevector-length demo-data))
        (set! demoplayback #t)))
  (set! vl-vbl #f)
  (DrawPlayScreen)
  (SetupGameLevel)
  (PM_CheckMainMem)
  (VL_SetPalette gamepal)
  (set! screenfaded #f)
  (set! fizzlein #f)
  (set! demo-lasttimecount 0)
  (set! demo-deadline (time-monotonic))
  (let ((started (time-monotonic)))
    (let loop ()
      (when (and (= playstate ex_stillplaying)
                 (if demoplayback
                     (< demo-pointer demo-end)
                     (not (ref Keyboard sc_Escape))))
        (PlayLoop)
        (set! demo-frames (+ demo-frames 1))
        (unless demo-headless (IN_Yield))
        (loop)))
    (let ((elapsed (* 1000 (- (time-monotonic) started)))
          (checksum (demo-checksum)))
      (display (string-append "demo: frames=" (number->string demo-frames)
                              " ticks=" (number->string (* demo-frames DEMOTICS))
                              " ms=" (number->string elapsed)
                              (if demo-headless
                                  (string-append " fps="
                                    (if (> elapsed 0)
                                        (number->string (/ (round (/ (* 10000 demo-frames) elapsed)) 10))
                                        "n/a"))
                                  "")
                              " checksum=" (number->string checksum) "\n"))
      (when demorecord (demo-save checksum))))
  (set! demorecord #f)
  (set! demoplayback #f))

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
      (plus-update-output)
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
