MODULE_CC += modules/dos/dos.cc
MODULE_CPP += modules/dos/dos.h
MODULE_PRELUDE += modules/dos/dos.ss
MODULE_SOKOL_CC += vendor/sokol/sokol_impl.cc
MODULE_SOKOL_LIBS := 1
MODULE_INIT += init_dos
MODULE_TESTS += ../modules/dos/tests/sound.ss
