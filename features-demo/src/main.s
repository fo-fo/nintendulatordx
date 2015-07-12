; Demonstration of various features of NintendulatorDX.
; by thefox//aspekt 2012

; Many of the macros require the DEBUG symbol to be defined.
; Typically you would have this in your Makefile, but for simplicity in this demo
; it's here.
DEBUG = 1
.include "ndxdebug.h"

.zeropage
; Try typing "foo" and "bar" in the Watch part of the debugger to observe
; the values of these variables.
foo:                .byte 0
bar:                .word 0
lua_button_pressed: .byte 0

.bss

.code

.proc reset
    jsr startCodeProfilingDemo
    jsr initVariables
    jsr initPalette
    jsr cycleCountingTimersDemo
    jsr debugOutputDemo
    jsr stopCodeProfilingDemo
    jsr luaDemo
    jsr debugBreakDemo

    jmp *
.endproc

.proc initVariables
    lda #123
    sta foo
    lda #222
    sta bar
    lda #111
    sta bar + 1
    lda #0
    sta lua_button_pressed
    rts
.endproc

.proc initPalette
    jsr pollVblank
    jsr pollVblank
    lda #$3F
    sta $2006
    ldx #0
    stx $2006
    lda #$0B
    sta $2007
    stx $2006
    stx $2006
    rts
.endproc

.proc pollVblank
    bit $2002
    poll:
        bit $2002
    bpl poll
    rts
.endproc

.proc startCodeProfilingDemo
    ; Start profiling all of the code executed from now on.
    ndxStartProfiling
    rts
.endproc

.proc stopCodeProfilingDemo
    ; Stop profiling. The results will be written out when the ROM is closed.
    ndxStopProfiling
    rts
.endproc

.proc cycleCountingTimersDemo
    ; Cycle counting timers are only available through registers.

    ; Start timer #3. The written value doesn't matter.
    sta $4023

    ; Kill some time.
    ldx #0
    more:
        dex
    bne more

    ; Stop timer #3. Debugger will then display the amount of elapsed cycles.
    sta $4033

    rts

    ; Set a title for the timer.
    __timer3_title:
      .asciiz "dummy loop"
.endproc

.proc debugOutputDemo
    ; Output a message to the Debug Information window using a macro.
    ndxDebugOut { "Value of foo is: $", ndxHex8 { foo } }

    ; Same thing as above, but now using a register (compatible with every
    ; assembler). Could be wrapped in a macro, too, but will always take
    ; space in the ROM, unlike ndxDebugOut.
    sta $4040
    jmp over
        .byte "Value of bar is: ", ndxDec16 { bar }, 0
    over:

    rts
.endproc

.proc luaDemo
    lda #123

    ; print() function prints to the Lua console.
    ndxLuaExecStr "print( 'In Lua, and A is ' .. REG.A )"

    ; NDX.print() prints to the Debug Information window.
    ndxLuaExecStr "NDX.print( 'Printing to the Debug Information window from Lua!' )"

    ; Now execute a Lua source file. Add an extra NOP by using the
    ; second parameter. This is needed because otherwise the ndxLuaExecFile
    ; would be at the same address as the "wait" label (and would be triggered
    ; by the loop).
    ndxLuaExecFile "demo.lua", 1

    ; Wait until the Lua scripts sets the lua_button_pressed
    ; variable as 1.
    wait:
        lda lua_button_pressed
    beq wait

    ndxLuaExecStr "print( 'Looks like you pressed the button!' )"

    rts
.endproc

.proc callbackTest
    ; This function is called from Lua code. We can call back to Lua again
    ; from here.

    ndxLuaExecStr "print( 'In callbackTest!' )"
    ndxLuaExecStr "luaCallbackTest( 12345 )"

    lda #111

    rts
.endproc

.proc callbackTest2
    nop
    nop
    nop

    ndxLuaExecStr "print( 'In callbackTest2!' )"

    nop
    nop
    nop

    rts
.endproc

.proc debugBreakDemo
    ; Finally, ndxDebugBreak can be used to stop the emulator at the specified point.
    lda #66
    ndxDebugBreak
    lda #55
    rts
.endproc

.proc nmi
    rti
.endproc

.proc irq
    rti
.endproc

.segment "CHR"
    .res 8192, 0

.segment "VECTORS"
    .word nmi
    .word reset
    .word irq
