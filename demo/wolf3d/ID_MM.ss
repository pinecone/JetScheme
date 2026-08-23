(define numUMBs 0)
(define mmstarted #f)
(define bombonerror #t)
(define mmerror #f)
(define bufferseg #f)
(define beforesort #f)
(define aftersort #f)
(define mminfotype (struct 'mminfotype '(nearheap farheap EMSmem XMSmem mainmem)))
(define mminfo (mminfotype 0 0 0 0 0))

(define (MML_CheckForXMS)
  (set! numUMBs 0)
  #f)

(define (MML_SetupXMS)
  #f)

(define (MML_ShutdownXMS)
  #f)

(define (MML_UseSpace segstart seglength)
  #f)

(define (MML_ClearBlock)
  #f)

(define (MM_Startup)
  (when mmstarted
    (MM_Shutdown))
  (set! mmstarted #t)
  (set! bombonerror #t)
  (set! mmerror #f)
  (set! mminfo (mminfotype 0 0 0 0 0))
  (set! bufferseg (make-bytevector 4096 0)))

(define (MM_Shutdown)
  (when mmstarted
    (set! bufferseg #f)))

(define (MM_GetPtr size)
  (set! mmerror #f)
  (make-bytevector size 0))

(define (MM_FreePtr pointer)
  #f)

(define (MM_SetPurge pointer purge)
  #f)

(define (MM_SetLock pointer locked)
  #f)

(define (MM_SortMem)
  (let ((playing (SD_SoundPlaying)))
    (when playing
      (MM_SetLock (ref audiosegs playing) #t))
    (SD_StopSound)
    (when beforesort
      (beforesort))
    (when aftersort
      (aftersort))
    (when playing
      (MM_SetLock (ref audiosegs playing) #f))))

(define (MM_ShowMemory)
  (VL_FadeIn 0 255 gamepal 30)
  (IN_Ack))

(define (MM_DumpData)
  (let ((port (open-output-file "MMDUMP.TXT")))
    (close-output-port port))
  (Quit "MMDUMP.TXT created."))

(define (MM_UnusedMemory)
  0)

(define (MM_TotalFree)
  0)

(define (MM_BombOnError bomb)
  (set! bombonerror bomb))
