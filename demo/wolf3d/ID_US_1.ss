;;; ID_US_1.C — table based random numbers

(define MaxX 320)
(define MaxY 200)

(define rndtable
  #u8(0 8 109 220 222 241 149 107 75 248 254 140 16 66
      74 21 211 47 80 242 154 27 205 128 161 89 77 36
      95 110 85 48 212 140 211 249 22 79 200 50 28 188
      52 140 202 120 68 145 62 70 184 190 91 197 152 224
      149 104 25 178 252 182 202 182 141 197 4 81 181 242
      145 42 39 227 156 198 225 193 219 93 122 175 249 0
      175 143 70 239 46 246 163 53 163 109 168 135 2 235
      25 92 20 145 138 77 69 166 78 176 173 212 166 113
      94 161 41 50 239 49 111 164 70 60 2 37 171 75
      136 156 11 56 42 146 138 229 73 146 77 61 98 196
      135 106 63 197 195 86 96 203 113 101 170 247 181 113
      80 250 108 7 255 237 129 226 79 107 112 166 103 241
      24 223 239 120 198 58 60 82 128 3 184 66 143 224
      145 224 81 206 163 45 63 90 168 114 59 33 159 95
      28 139 123 98 125 196 15 70 194 253 54 14 109 226
      71 17 161 93 186 87 244 138 20 52 123 251 26 36
      17 46 52 231 232 76 31 221 84 37 216 165 212 106
      197 242 98 43 39 175 254 145 190 84 118 222 187 136
      120 163 236 249))

(define rndindex 0)
(define US_Started #f)
(define compatability #f)
(define tedlevel #f)
(define tedlevelnum 0)
(define NoWait #f)
(define ParmStrings (vector "TEDLEVEL" "NOWAIT"))
(define ParmStrings2 (vector "COMP" "NOCOMP"))

(define (USL_HardError errval ax bp si) 2)

(define (US_InitRndT start)
  (set! rndindex (bitwise-and start 255)))

(define (US_Startup)
  (unless US_Started
    (US_InitRndT 1)
    (let scan-compatability ((index 1))
      (when (< index (vector-length argv))
        (let ((match (US_CheckParm (ref argv index) ParmStrings2)))
          (cond ((= match 0) (set! compatability #t))
                ((= match 1) (set! compatability #f))))
        (scan-compatability (+ index 1))))
    (let scan-launch ((index 1))
      (when (< index (vector-length argv))
        (let ((match (US_CheckParm (ref argv index) ParmStrings)))
          (cond ((= match 0)
                 (set! tedlevelnum (string->number (ref argv (+ index 1))))
                 (when (>= tedlevelnum 0)
                   (set! tedlevel #t)))
                ((= match 1) (set! NoWait #t))))
        (scan-launch (+ index 1))))
    (set! US_Started #t)))

(define (US_Shutdown)
  (when US_Started
    (set! US_Started #f)))

(define (US_CheckParm parm strings)
  (let skip ((offset 0))
    (if (char-alphabetic? (ref parm offset))
        (let find ((index 0))
          (if (= index (vector-length strings))
              -1
              (let ((candidate (ref strings index)))
                (let compare ((candidate-offset 0) (parm-offset offset))
                  (if (= candidate-offset (string-length candidate))
                      index
                      (if (or (>= parm-offset (string-length parm))
                              (not (char=? (char-downcase (ref candidate candidate-offset))
                                           (char-downcase (ref parm parm-offset)))))
                          (find (+ index 1))
                          (compare (+ candidate-offset 1) (+ parm-offset 1))))))))
        (skip (+ offset 1)))))

(define (US_RndT)
  (set! rndindex (bitwise-and (+ rndindex 1) 255))
  (ref rndtable rndindex))

(define MaxString 128)
(define USL_MeasureString (lambda (text) (VW_MeasurePropString text)))
(define USL_DrawString (lambda (text) (VW_DrawPropString text)))

(define (US_SetPrintRoutines measure print)
  (set! USL_MeasureString measure)
  (set! USL_DrawString print))

;; ID_US_1.C:39-40
(define PrintX 0)
(define PrintY 0)
(define WindowX 0)
(define WindowY 0)
(define WindowW 320)
(define WindowH 200)
(define backcolor 0)

(define (line-end text start)
  (let scan ((index start))
    (cond ((= index (string-length text)) index)
          ((char=? (string-ref text index) #\newline) index)
          (else (scan (+ index 1))))))

;; ID_US_1.C:288-317
(define (US_Print text)
  (let loop ((start 0))
    (when (< start (string-length text))
      (let* ((stop (line-end text start))
             (segment (substring text start stop))
             (size (USL_MeasureString segment)))
        (set! px PrintX)
        (set! py PrintY)
        (USL_DrawString segment)
        (if (< stop (string-length text))
            (begin
              (set! PrintX WindowX)
              (set! PrintY (+ PrintY (second size)))
              (loop (+ stop 1)))
            (set! PrintX (+ PrintX (first size))))))))

;; ID_US_1.C:325-330
(define (US_PrintUnsigned value)
  (US_Print (number->string (modulo value 4294967296))))

(define (US_PrintSigned value)
  (let ((wrapped (modulo value 4294967296)))
    (US_Print (number->string (if (>= wrapped 2147483648) (- wrapped 4294967296) wrapped)))))

(define (USL_PrintInCenter text rect)
  (let ((size (USL_MeasureString text))
        (left (ref rect 0))
        (top (ref rect 1))
        (right (ref rect 2))
        (bottom (ref rect 3)))
    (set! px (+ left (truncate (/ (- (- right left) (first size)) 2))))
    (set! py (+ top (truncate (/ (- (- bottom top) (second size)) 2))))
    (USL_DrawString text)))

(define (US_PrintCentered text)
  (USL_PrintInCenter text (vector WindowX WindowY (+ WindowX WindowW) (+ WindowY WindowH))))

;; ID_US_1.C:389-401
(define (US_CPrintLine text)
  (let ((size (USL_MeasureString text)))
    (when (> (first size) WindowW)
      (Quit "US_CPrintLine() - String exceeds width"))
    (set! px (+ WindowX (truncate (/ (- WindowW (first size)) 2))))
    (set! py PrintY)
    (USL_DrawString text)
    (set! PrintY (+ PrintY (second size)))))

;; ID_US_1.C:410-431
(define (US_CPrint text)
  (let loop ((start 0))
    (when (< start (string-length text))
      (let ((stop (line-end text start)))
        (US_CPrintLine (substring text start stop))
        (loop (+ stop 1))))))

;; ID_US_1.C:436-446
(define (US_ClearWindow)
  (VWB_Bar WindowX WindowY WindowW WindowH 15)
  (set! PrintX WindowX)
  (set! PrintY WindowY))

(define (US_DrawWindow x y width height)
  (set! WindowX (* x 8))
  (set! WindowY (* y 8))
  (set! WindowW (* width 8))
  (set! WindowH (* height 8))
  (set! PrintX WindowX)
  (set! PrintY WindowY)
  (let ((start-x (* (- x 1) 8))
        (start-y (* (- y 1) 8))
        (outer-width (* (+ width 1) 8))
        (outer-height (* (+ height 1) 8)))
    (US_ClearWindow)
    (VWB_DrawTile8 start-x start-y 0)
    (VWB_DrawTile8 start-x (+ start-y outer-height) 5)
    (let horizontal ((draw-x (+ start-x 8)))
      (if (<= draw-x (- (+ start-x outer-width) 8))
          (begin
            (VWB_DrawTile8 draw-x start-y 1)
            (VWB_DrawTile8 draw-x (+ start-y outer-height) 6)
            (horizontal (+ draw-x 8)))
          (begin
            (VWB_DrawTile8 draw-x start-y 2)
            (VWB_DrawTile8 draw-x (+ start-y outer-height) 7))))
    (let vertical ((draw-y (+ start-y 8)))
      (when (<= draw-y (- (+ start-y outer-height) 8))
        (VWB_DrawTile8 start-x draw-y 3)
        (VWB_DrawTile8 (+ start-x outer-width) draw-y 4)
        (vertical (+ draw-y 8))))))

(define (US_SaveWindow)
  (tuple WindowX WindowY WindowW WindowH PrintX PrintY))

(define (US_RestoreWindow window)
  (set! WindowX (ref window 0))
  (set! WindowY (ref window 1))
  (set! WindowW (ref window 2))
  (set! WindowH (ref window 3))
  (set! PrintX (ref window 4))
  (set! PrintY (ref window 5)))

(define (US_CenterWindow width height)
  (US_DrawWindow (truncate (/ (- (truncate (/ MaxX 8)) width) 2))
                 (truncate (/ (- (truncate (/ MaxY 8)) height) 2))
                 width height))

;; WL_INTER.C:1101. C adds 129-'0' to each digit in place; a Jet string is UTF-8, so the
;; fixed-width glyphs are carried and drawn as codes.
(define (fixed-codes text)
  (let loop ((index (- (string-length text) 1)) (out '()))
    (if (< index 0)
        out
        (loop (- index 1) (cons (+ 81 (char->integer (string-ref text index))) out)))))

(define (US_MeasureCodes codes)
  (let ((font (VH_Font)))
    (let loop ((rest codes) (total 0))
      (if (null? rest)
          total
          (loop (cdr rest) (+ total (font-width font (car rest))))))))

(define (US_PrintCodes codes)
  (let ((font (VH_Font)))
    (set! px PrintX)
    (set! py PrintY)
    (let loop ((rest codes))
      (unless (null? rest)
        (VW_DrawPropChar font (car rest))
        (loop (cdr rest))))
    (set! PrintX px)))

(define cursor-status #f)

;; ID_US_1.C:537-560. Glyph 128 is the I-bar; it is drawn by code, because a Jet string holds
;; UTF-8 and would carry it as two bytes.
(define (USL_XORICursor x y text cursor)
  (let ((size (USL_MeasureString (substring text 0 cursor))))
    (set! px (+ x (first size) -1))
    (set! py y)
    (set! cursor-status (not cursor-status))
    (if cursor-status
        (VW_DrawPropChar (VH_Font) 128)
        (let ((temp fontcolor))
          (set! fontcolor backcolor)
          (VW_DrawPropChar (VH_Font) 128)
          (set! fontcolor temp)))))

(define (string-remove text at)
  (string-append (substring text 0 at) (substring text (+ at 1) (string-length text))))

(define (string-insert text at code)
  (string-append (substring text 0 at)
                 (string (integer->char code))
                 (substring text at (string-length text))))

(define (USL_FinishLineInput x y result olds)
  (unless result
    (set! px x)
    (set! py y)
    (USL_DrawString olds))
  (VW_UpdateScreen)
  (IN_ClearKeysDown)
  result)

;; ID_US_1.C:576-756. C copies the result into buf; here the accepted string is the return
;; value and escape returns #f. A headless script installs no yield, so the wait ends at once.
(define (US_LineInput x y def escok maxchars maxwidth)
  (set! LastASCII key_None)
  (set! LastScan sc_None)
  (let loop ((s (if def def ""))
             (olds "")
             (cursor (string-length (if def def "")))
             (redraw #t)
             (cursorvis #f)
             (cursormoved #t)
             (lasttime TimeCount))
    (when cursorvis
      (USL_XORICursor x y s cursor))
    (let ((sc LastScan)
          (code LastASCII)
          (length-of (string-length s)))
      (set! LastScan sc_None)
      (set! LastASCII key_None)
      (cond
        ((= sc sc_Return) (USL_FinishLineInput x y s olds))
        ((and (= sc sc_Escape) escok) (USL_FinishLineInput x y #f olds))
        (else
         (let* ((moved (or (= sc sc_LeftArrow) (= sc sc_RightArrow)
                           (= sc sc_Home) (= sc sc_End)
                           (= sc sc_BackSpace) (= sc sc_Delete)))
                (eaten (or moved (= sc 76) (= sc sc_UpArrow) (= sc sc_DownArrow)
                           (= sc sc_PgUp) (= sc sc_PgDn) (= sc sc_Insert)
                           (= sc sc_Escape)))
                (typed (if eaten key_None code))
                (edited
                 (cond ((and (= sc sc_BackSpace) (> cursor 0)) (string-remove s (- cursor 1)))
                       ((and (= sc sc_Delete) (< cursor length-of)) (string-remove s cursor))
                       (else s)))
                (moved-cursor
                 (cond ((and (= sc sc_LeftArrow) (> cursor 0)) (- cursor 1))
                       ((and (= sc sc_RightArrow) (< cursor length-of)) (+ cursor 1))
                       ((= sc sc_Home) 0)
                       ((= sc sc_End) length-of)
                       ((and (= sc sc_BackSpace) (> cursor 0)) (- cursor 1))
                       (else cursor)))
                (fits (and (>= typed 32) (<= typed 126)
                           (< (string-length edited) (- MaxString 1))
                           (or (= maxchars 0) (< (string-length edited) maxchars))
                           (or (= maxwidth 0) (< (first (USL_MeasureString edited)) maxwidth))))
                (next-s (if fits (string-insert edited moved-cursor typed) edited))
                (next-cursor (if fits (+ moved-cursor 1) moved-cursor))
                (next-redraw (or redraw fits (not (string=? edited s)))))

           (when next-redraw
             (set! px x)
             (set! py y)
             (let ((temp fontcolor))
               (set! fontcolor backcolor)
               (USL_DrawString olds)
               (set! fontcolor temp))
             (set! px x)
             (set! py y)
             (USL_DrawString next-s))

           (let* ((next-olds (if next-redraw next-s olds))
                  (blink-base (if (or cursormoved moved) (- TimeCount TickBase) lasttime))
                  (visible (if (or cursormoved moved) #f cursorvis))
                  (blink (> (- TimeCount blink-base) (truncate (/ TickBase 2))))
                  (next-vis (if blink (not visible) visible))
                  (next-time (if blink TimeCount blink-base)))
             (when next-vis
               (USL_XORICursor x y next-s next-cursor))
             (VW_UpdateScreen)
             (IN_Yield)
             (loop next-s next-olds next-cursor #f next-vis #f next-time))))))))
