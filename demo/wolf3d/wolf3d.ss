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

(CheckForEpisodes)
(Patch386)
(InitGame)

(define title-frames 0)
(define title-time (time-monotonic))

(define (update-title)
  (set! title-frames (+ title-frames 1))
  (let ((elapsed (- (time-monotonic) title-time)))
    (when (>= elapsed 1)
      (dos:set-window-title
       (string-append "Wolfenstein 3D - "
                      (number->string (/ (round (/ (* 10 title-frames) elapsed)) 10))
                      " fps"))
      (set! title-frames 0)
      (set! title-time (+ title-time elapsed)))))

;; sokol allows one image update per frame, so only the driver below displays a framebuffer.
(set! vl-vbl (lambda (count) (IN_Yield)))

;; The game runs in a coroutine, so a key wait yields one frame instead of
;; blocking the frame callback.
(define game
  (let/coro yield ()
    (set! in-yield yield)
    (PG13)
    (let outer ()
      (set! ingame #f)
      (DemoLoop)
      (set! ingame #t)
      (DrawPlayScreen)
      (if loadedgame
          (begin
            (DrawLevel)
            (set! playstate ex_stillplaying))
          (start-level))
      (let play ()
        (if (game-step)
            (outer)
            (begin
              (yield #f)
              (play)))))))

(define (frame)
  (IN_PollKeyboard)
  (CalcTics)
  (set! TimeCount (+ TimeCount tics))
  (coro/next game #f)
  ;; The host presents the current visible linear RAM image.  The explicit
  ;; display page remains for reference copies, but direct reference writes
  ;; (including FizzleFade's per-VBL steps) must be visible immediately.
  (dos:display-framebuffer framebuffer)
  (update-title))

(dos:frame-loop "Wolfenstein 3D" screenwidth screenheight frame)
