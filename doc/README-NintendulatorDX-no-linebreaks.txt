
                       -----------------
                        NintendulatorDX
                       -----------------

                              v36

                       by thefox//aspekt
                        thefox@aspekt.fi
                     http://kkfos.aspekt.fi

               Based on Nintendulator SVN revision
           Nintendulator Copyright (C) QMT Productions


General
-------

This is my modified version of the Nintendulator NES emulator, which adds source-level debugging capabilities (among other features) to the debugger assuming the ROM was assembled with (a dev version) of CC65 with debugging info enabled.

The goal of this project is to make NES *development* easier. If you just want to play some games, you shouldn't use it!

Most features rely on the debug file output by CA65, but some of them will also work on other assemblers by using memory mapped registers. The memory mapped registers will be called "legacy" in this README, because a better way to use the features is to use the provided macros (see "NDX Macros").

Note! A recent snapshot version of CA65 is required, it's available at http://sourceforge.net/projects/cc65/files/

Features:
  * Source-level debugging of NES ROMs compiled with CC65 (CA65)
  * Watches for 8-bit and 16-bit variables
  * Cycle counting timers (with optional titles)
    * Automatically counts cycles of NSF Init and Play routines
  * Debug output strings, with support for 8-bit and 16-bit variables
  * Automatic code profiling (based on .proc/.scope blocks within the source code)
  * Memory (RAM, PRG-RAM, VRAM, OAM, palette) can be randomized on power-on
  * Diagnostic messages about uninitialized memory usage
  * Lua scripting
    * Lua code can be executed from within the ROM for practically endless options (runtime asserts, code prototyping, debugging, custom GUIs, etc.)
    * IUP GUI toolkit support for user interfaces
    * CD graphics library support for drawing on the emulation window

A CA65 demo of all the features can be found in the directory "Demo".

Note! Readme.txt and savestate.txt are from Nintendulator 0.970, and may be outdated.

Source code is available at https://github.com/fo-fo/nintendulatordx


Source-level Debugging
----------------------

Source-level debugging means you're able to view the original source file (and the current location of execution in it) when debugging your program, instead of just the disassembly provided by the emulator.

For this feature to work, you need to include debug information in object files by passing the "-g" switch to cl65/ca65. When linking, you need to pass the "--dbgfile foo.nes.dbg" switch to the linker to generate a debug file. The name of the debug file must correspond to the name of the iNES ROM file (with ".dbg" extension added).

This feature will only work if the linker generates the iNES file in one go. In other words, you may not generate parts of it (like the header, or differet banks) separately, and then combine them later, as this will mess up the banking calculations.


Watches
-------

Watches allow you to watch values of symbolic variables defined in your 6502 assembly code. Simply type the name of the variable in the text box under "Watch" in the Debugger window and press ENTER.

NintendulatorDX will try to automatically detect the size of the variable (if it was declared with .byte, word or .res 2 and so on), but if you want to force a certain size, append ",b" or ",w" to the variable name.

Display format is as follows:

      reset,b        $8053 (L) = $A2 (162)
      |     |          |    |     |    |__ Decimal value
      |     |          |    |     |
      |     |          |    |     |__ Hexadecimal value
      |     |          |    |
      |     |          |    |__ (L)abel/(E)quate
      |     |          |
      |     |          |__ Hexadecimal address
      |     |
      |     |__ Size of the variable (b or w)
      |
      |__ Name of the variable


Known problems:
  * If the variable name is too long, only the name of the variable is visible in the watch list.


Cycle Counting Timers
---------------------

16 CPU cycle counting timers are available for timing different parts of your code. The timers can only be used by writing to a register at the moment. They work by writing to register $402x when you want the timing to start, and to $403x when you want it to end. "x" is the timer number.

E.g.

  ; Start timer #5. The written value doesn't matter.
  sta $4025
  ; Do whatever.
  nop
  lda #123
  ; Stop timer #5. After this the Debugger window will display the
  ; number of CPU cycles taken by the above code block (4 cycles).
  sta $4035

The number of cycles taken by the STA instructions do not count into the reported number of cycles!

By defining a specially named symbol it's possible to define a name for the timer. The symbol name should be "__timerN_title", and it should point to a zero terminated string containing the name of the timer (replace N with the timer number). This name is displayed in the Debugger window.

E.g.

  .code
    __timer5_title:
      .asciiz "nmi routine"

The following macros may be useful:

  .macro startTimer timer
    sta $4020 + timer
  .endmacro

  .macro stopTimer timer
    sta $4030 + timer
  .endmacro

  .macro timerTitle timer, title
    .ident( .sprintf( "__timer%d_title", timer ) ):
      .asciiz title
  .endmacro

Known problems:
  * The address of the registers ($402x/$403x) overlaps the FDS register area.

See also:
  * [Lua] NDX.getCPUCycles()


Debug Output
------------

This feature can be used to output strings (and values of variables) to the Debug Information window. ndxDebugOut macro or a legacy register write ($4040) can be used.

E.g.

  .zeropage
    foo: .byte 0

  .code
    ndxDebugOut { "Value of foo is: ", ndxHex8 { foo } }

Note that the whole string has to be a single parameter, and thus has to be wrapped inside curly braces. Also note that "foo" can be replaced by any expression that the assembler can parse at compile time.

The available formatting macros are ndxHex8, ndxHex16, ndxDec8 and ndxDec16.

For an example on how to use the register $4040 for debug output, see the feature demo in the "Demo" directory.

See also:
  * [Lua] NDX.print(), print()


Automatic Code Profiling
------------------------

This feature automatically gathers information about how many CPU cycles have been spent executing each .proc/.scope block defined in the program. ndxStartProfiling and ndxStopProfiling macros can be used. The corresponding legacy registers are $4041 and $4042.

If you don't call ndxStopProfiling (or write to $4042) the profiling will stop when the ROM is unloaded.

The profiling data is written when the ROM is unloaded (for example when the emulator is closed) to a file named "foo.nes.profiling.txt" where "foo.nes" is the filename of your ROM.

The output is something like this:

  [module general.o]: 4328012 (91.6 %)
    init: 89352 (2.06 %)
    detectSystem: 60556 (1.4 %)
    initVariables: 44 (0.00102 %)
    reset: 2771377 (64 %)
    setPalette: 24 (0.000555 %)
    updateControllerState: 4903 (0.113 %)
    nmi: 1401756 (32.4 %)
    irq: 0 (0 %)

This tells us that 91.6% of processing time was spent in the module general.o, and out of those 4328012 cycles 1.4% was spent in the routine detectSystem, and so on. In other words, the percentages on the higher levels are always relative to the parent.


NDX Macros
----------

These macros provide access to some of the debug features without embedding any code (*1) in the ROM. They work by generating a specially named scope, which the emulator can find from the debug file. Since many of the macros require string parameters (like debug output), they are output to a file with a ".ndx" extension (e.g. "foo.nes.ndx" if the ROM is "foo.nes").

These macros are available in the header file "ndxdebug.h" (in the directory Demo/src). You also need to specify a segment named "NDXDEBUG" and a corresponding MEMORY definition for it in your CC65 (LD65) linker configuration file. Add these specifications at the end of your MEMORY and SEGMENTS blocks:

  memory {
    NDXDEBUG:
      start = 0,
      # 256 MB should be enough for everybody.
      size  = $10000000,
      type  = ro,
      file  = "foo.nes.ndx";
  }

  segments {
    NDXDEBUG:
      load      = NDXDEBUG,
      type      = ro,
      optional  = yes;
  }

Available macros:
  * ndxDebugOut <message>
    Print a message in the Debug Information window
  * ndxDebugBreak
    Break in to the debugger
  * ndxStartProfiling
    Start .proc/.scope based profiling
  * ndxStopProfiling
    Stop .proc/.scope based profiling
  * ndxLuaExecStr <string>
    Execute a string literal as Lua code
  * ndxLuaExecFile <filename>
    Execute a Lua source file

---
(*1) In some cases it's necessary to output a single NOP for the emulator to be able to unambiguosly determine the address where the macro was called. This can be achieved by setting the "output_nop" parameter of the macro to 1:

  beq foo
    ; Because "foo" has the same address as where the macro is called,
    ; without the extra NOP the debug output would be printed regardless
    ; of whether the branch is taken or not.
    ndxDebugOut "Zero flag not set", 1
  foo:


Lua Scripting
-------------

Lua scripting support is probably the most powerful feature of NintendulatorDX. It can be used to implement functionality like runtime asserts, conditional breakpoints, debug output, custom GUIs, as well as used for quick prototyping. From emulators point of view Lua code is always executed in 0 cycles. Needless to say, Lua scripting is a emulator specific feature and will never work the same way on the real NES hardware.

Lua code can be called in two ways, from within the ROM (using ndxLuaExecStr or ndxLuaExecFile) or from the Lua Console.

When a ROM is loaded, NintendulatorDX will execute the file "init.lua" in the same directory as Nintendulator.exe. The initialization script opens the Lua Console window regardless of whether the ROM uses any Lua code.

In the Lua Console window, press Browse to browse for a Lua source file, press Run to run the file, and Pause to disable all hooks (callbacks) temporarily.

When you press Run (and if the Lua script is valid), you should see this message in the Lua Console:

  (Lua Console) Compiling and executing 'foo.lua'...
  (Lua Console) Execution finished succesfully.

Even though the execution of the module finished, it's possible (and likely) that the script installed hooks that will be triggered on certain events. Currently the only available event is the end of the frame (NDX.setAfterFrameHook()). Script may also have opened a GUI window. This way the script can stay active even after the execution of the file has finished.

ndxLuaExecStr can be used as follows:

  ndxLuaExecStr "print( 'In Lua, and A is ' .. NDX.getRegister( 'A' ) )"

Note the use of single quotes for strings in the Lua code.

ndxLuaExecFile is equivalently simple:

  ndxLuaExecFile "foo.lua"

The emulator searches for the Lua source file in the same directory as where the assembly source is.

Lua state is (at the moment) reset only when the ROM is reloaded, so you must make sure that your script behaves well when executed multiple times (if that's a possibility, e.g. when you have Lua code embedded in your 6502 initialization code).

It's important to understand that when Lua code is executed using ndxLuaExecStr or ndxLuaExecFile, or from a hook callback, the emulation thread is completely blocked. The reason is to be able to provide consistent access to the emulator state (memory, registers). At the moment there's no way to "relinquish" control to the emulator thread (even if your code doesn't need to access the emulator state), so you should make sure that your code doesn't take overly long time or the FPS will take a hit.

If you use the IUP GUI functions, make sure to read the warning in the "Lua GUI Functions" chapter.

Note! NintendulatorDX replaces the standard Lua require() function with a function that looks for the include files in the directory where the source file is (in addition to the standard methods used by require()).

See also:
  * Lua Function Reference


Lua Function Reference
----------------------

Some of the functions are exported from the emulator and some are defined in "init.lua" file. Here are the available functions/tables (in addition to the Lua standard library and IUP and CD libraries):

* print( string )
  Prints to the Lua Console. Overrides the default print() function of Lua.

* RAM
  This table can be used to easily access the NES RAM contents as bytes. The index can be a string (a symbol) or a number, for example:

    -- Get the byte value of the symbol "foo".
    foo = RAM.foo
    -- Get the byte value from address $123.
    bar = RAM[ 0x123 ]
    -- Set the value of "foo".
    RAM.foo = 111

  If more than one symbols are defined with the same name, the table will raise an error when accessing the symbol.

* SYM
  This table provides easy access to debug symbol values. It's read only. Scoping is not taken in account. The results are returned as a table, which may contain multiple values.

  E.g.

    print( string.format( "Address of foo is %X", SYM.foo[ 1 ] ) )

* REG
  This table provides easy access to the registers and the flags of NES. Registers are returned as numbers and flags are returned as booleans. Parameter (case-insensitive) has to be one of: A, X, Y, SP, P, PC, C, Z, I, D, V, N

  E.g.

    print( string.format( "A = %X, C = %s", REG.A, REG.C ) )

* NDX.print( string )
  Prints to the Debug Information window.

* NDX.readRAM( address )
  Reads a byte from RAM.

* NDX.writeRAM( address, value )
  Writes a byte to RAM.

* NDX.readMemory( address, value )
  Reads a byte from the 6502 memory space. This function can read RAM, PRG-RAM and PRG-ROM, unlike readRam() which can only read RAM. Nil is returned if the address couldn't be read (e.g. PPU registers).

* NDX.getSymbolValueByName( symbol_name )
  Gets a symbol value from debug information as a table (may contain multiple values).

  See also: SYM, RAM

* NDX.getPPUCycles()
  Gets the number of PPU cycles elapsed since reset.

* NDX.getCPUCycles()
  Gets the number of CPU cycles elapsed since reset.

* NDX.stop()
  Stops the emulator. Returns a boolean indicating whether the emulator was running before the function was called.

* NDX.run()
  Starts the emulator. Returns a boolean indicating whether the emulator was stopped before the function was called.

* NDX.getRegister( reg_name )
  Gets the value of a register/flag.

  See also: REG

* NDX.setRegister( reg_name, value )
  Sets the value of a register/flag.

  See also: REG

* NDX.setAfterFrameHook( hook_function )
  Sets a hook function to run after the frame has finished rendering (specifically right after the last clock cycle of 240th visible scanline).

  The function replaces the current hook function with the parameter and returns the old one (or nil). If you want to have multiple hook functions, you'll have to chain them by calling the old function from the new handler.

  The hook function can return a CD client canvas (cd.IMAGERGB) that will then be drawn on top of the emulation window. For more details see the chapter "Lua Graphics Overlays"

* NDX.getMouseState()
  Gets the current state of the mouse as 5 return values: x, y (numbers) and left, right, middle (booleans).

  E.g.

    x, y, left, right, middle = NDX.getMouseState()
    if right then
      print( "Right mouse button is being pressed" )
    end

* NDX.getControllerState( port )
  Returns the state of the standard controller on given port (1 or 2) as a single 8-bit number. If the controller isn't connected, returns nil.

  Lua's standard bit32 library can be used to test the bits:

    state_bits = NDX.getControllerState( 1 )
    if state_bits and bit32.btest( state_bits, 0x08 ) then
      print( "Start button is being pressed on controller 1" )
    end


Lua Graphics Overlays
---------------------

An "after frame" hook function (set with NDX.setAfterFrameHook()) can return a CD graphics library canvas to overlay it on top of the emulation window.

Note! Graphics overlays currently only work when the NintendulatorDX surface is 32-bit (i.e. your desktop color depth is 32 bits). Also the "scanlines" feature must be disabled.

The canvas has to be of type "cd.IMAGERGB", it has to be 256x240 pixels in size with a bit depth of 32 bits, e.g.

  hook_canvas = cd.CreateCanvas( cd.IMAGERGB, "256x240 -a" )

Because the canvas is alpha blended on top of the emulation window, it's a good idea to set its background color to invisible:

  hook_canvas:SetBackground( cd.EncodeAlpha( cd.BLACK, 0 ) )

You can use any of the available CD functions to draw on this canvas, then return it from your hook function, and you're good to go.

Note! CD's coordinate system places the origo at the bottom left corner of the image, and Y coordinate grows upwards, so if you want to convert from a coordinate that assumes origo at top left corner and Y grows downwards, use this formula:

  cd_y = -y + 239

Alternative way to fix the coordinates without having to manually convert them is to set the transformation matrix, but this method has the problem that it flips the text upwards:

  hook_canvas:Transform { 1, 0, 0, -1, 0, 239 }

The "fceux-compat.lua" file may give you some idea of the available drawing functions. Otherwise, the documentation for CD is available at http://www.tecgraf.puc-rio.br/cd/

CD is automatically loaded so you don't have to require() it.


Lua GUI Functions
-----------------

GUI functions are provided by the IUP GUI toolkit. Documentation is at http://www.tecgraf.puc-rio.br/iup/

Warning! If you use IUP event handlers (like a button press), the emulator thread is NOT automatically stopped when the event handler is called. You have to manually stop it using NDX.stop() and resume it by using NDX.run() if you want to safely access the emulator state!

IUP is automatically loaded so you don't have to require() it.


Miscellaneous
-------------

* Check/uncheck the "Expand macros" checkbox in the Debugger window to control whether the Debugger will step inside macros.
* The menu option Debug -> Debug Extensions -> Randomize Memory causes RAM/PRG-RAM/VRAM/OAM/palette memory to be randomized on power-on. This allows you to test whether your program relies on uninitialized state. If the option is off, memory is cleared to 0. If the ROM uses battery-backed SRAM, it isn't cleared or randomized.
* A message is shown in the Debug Information window when uninitialized memory is accessed by the running program. If a save state is loaded, this feature is disabled (because save state doesn't contain information about memory access patterns). This feature can be entirely disabled from the menu Debug -> Debug Extensions -> Memory Warnings.


Version History
---------------

- v36 (2015-07-12)
  * NDX searches for source files in the current working directory (previously only relative to the ROM directory)
  * NDX searches for Lua scripts relative to current working directory.
  * Menu option added for NTSC/PAL aspect ratio.
  * NTSC filter support added (blargg's nes_ntsc).
  * Frame skip option removed.
  * Color depths other than 32-bit removed.
  * Diagnostic is shown if invalid black (0xD) is used in rendering.
  * Improved the implementation of Lua require().
  * Fixed a bug that caused the A register to be cleared when ndxDebugBreak was used without the debugger window being open.
  * Fixed some problems with POV axes (patch from Nintendulator main repository)
  * Lua code can call back into 6502 code (NDX.jsr())
  * Menu option for darkening borders outside the 90% safe area of the screen.


- v35 (2015-02-27)
  * "Expand macros" option.
  * CA65 debug header no longer tramples over custom character mappings.
  * Absolute file paths can be used in debug files.
  * Bug fixed from dbginfo.c that caused some (rare) debug files not to be loaded.
  * readMemory() function added to the Lua interface.
  * Latest updates from the main Nintendulator repo integrated.
  * Warnings are given for uninitialized PRG-RAM accesses.
  * Memory can be randomized on power-on.
  * Lua and IUP updated to the latest versions (CD not updated due to a bug in the latest version).
  * Updated project to Visual Studio 2013.
  * Added limited PowerPak hardware emulation (requires recompilation).
  * Diagnostic message is shown if PPU_DATA is written to during rendering.
  * NTSC NES borders are displayed.


- v34 (2012-11-08)
  * Version numbering method changed.
  * Added Lua scripting support.
  * Updated the mapper files.
  * Wrote a better README file.

(Version history for older versions not recorded.)
