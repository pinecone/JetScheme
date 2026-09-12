# Wolf3D

This is a port of Wolfenstein 3D to JetScheme. The game logic and software renderer are written entirely in Scheme and run on JetScheme's bytecode interpreter. YMFM natively emulates the AdLib synthesizer. JetScheme's `dos` module handles input, audio, framebuffer display, and super fun CRT monitor emulation.

After building JetScheme, you can run the (included!) shareware version of Wolf3D like this: 

```sh
./demo/wolf3d/run [--plus]
```

* `--plus` enables ✨Plus Mode✨, which adds a few graphical and other enhancements. When running in ✨Plus Mode✨, you can toggle it on/off using Shift+P.
* The port is also compatible with the full version of Wolf3D, featuring five additional sizzling episodes. you can pass its location via `--datadir BLAH`.


![JetScheme running Wolf3D](jetwolf.gif)

See `COPYING` for the source-code license.
