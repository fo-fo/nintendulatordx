.ifndef NDXDEBUG_H
NDXDEBUG_H = 1

.enum __NDXDebugType
    STRING
    BREAK
    START_PROFILING
    STOP_PROFILING
    LUA_EXEC_STR
    LUA_EXEC_FILE
.endenum

.macro __startNDXDebugInfo dbgtype
    .scope
    .scope __NDXDebug
        type = dbgtype
        PC = *
        .pushseg
        .segment "NDXDEBUG"
.endmacro

.macro __endNDXDebugInfo output_nop
        .popseg
    .endscope
    .endscope
    .ifnblank output_nop
        .if output_nop
            nop
        .endif
    .endif
.endmacro

.macro __saveCharMap
    ; In scope so that CHARMAP_xxxs are local.
    .scope
        ; Find out character mappings.
        .repeat 255, i
            ; Change mapping only if it's not already direct to avoid flooding the debug information with unnecessary symbols.
            .if i + 1 <> .strat( .sprintf( "%c", i + 1 ), 0 )
                ; Save the current mapping.
                .ident( .sprintf ("__CHARMAP_%d", i + 1 ) ) .set .strat( .sprintf( "%c", i + 1 ), 0 )
                ; Set mapping to direct (1..255 -> 1..255).
                .charmap i + 1, i + 1
            .endif
        .endrepeat
.endmacro

.macro __restoreCharMap
        ; Now restore the original mapping.
        .repeat 255, i
           ; Restore only those char mappings which were modified.
            .ifdef .ident( .sprintf ("__CHARMAP_%d", i + 1 ) )
                .charmap i + 1, .ident( .sprintf ("__CHARMAP_%d", i + 1 ) )
            .endif
        .endrepeat
    .endscope
.endmacro

; Output a string to the Debug Information window.
.macro ndxDebugOut str, output_nop
    .ifndef DEBUG
        .exitmac
    .endif
    
    __saveCharMap
    __startNDXDebugInfo __NDXDebugType::STRING
        string_start:
        .byte str
        string_len = * - string_start
    __endNDXDebugInfo output_nop
    __restoreCharMap
.endmacro

; Macros for formatting the ndxDebugOut output.
.define ndxHex8( addr ) 1, 0, <(addr), >(addr)
.define ndxDec8( addr ) 1, 1, <(addr), >(addr)
.define ndxHex16( addr ) 1, 2, <(addr), >(addr)
.define ndxDec16( addr ) 1, 3, <(addr), >(addr)

; Execute a string as Lua code.
.macro ndxLuaExecStr str, output_nop
    __saveCharMap
    __startNDXDebugInfo __NDXDebugType::LUA_EXEC_STR
        string_start:
        .byte str
        string_len = * - string_start
    __endNDXDebugInfo output_nop
    __restoreCharMap
.endmacro

; Execute a string as Lua code (only in DEBUG mode).
.macro ndxLuaExecStrDebug str, output_nop
    .ifndef DEBUG
        .exitmac
    .endif
    
    ndxLuaExecStr str, output_nop
.endmacro

; Execute a file as Lua code.
.macro ndxLuaExecFile str, output_nop
    __saveCharMap
    __startNDXDebugInfo __NDXDebugType::LUA_EXEC_FILE
        string_start:
        .byte str
        string_len = * - string_start
    __endNDXDebugInfo output_nop
    __restoreCharMap
.endmacro

; Execute a file as Lua code (only in DEBUG mode).
.macro ndxLuaExecFileDebug str, output_nop
    .ifndef DEBUG
        .exitmac
    .endif
    
    ndxLuaExecFile str, output_nop
.endmacro

; Stop the emulator.
.macro ndxDebugBreak output_nop
    .ifndef DEBUG
        .exitmac
    .endif

    __startNDXDebugInfo __NDXDebugType::BREAK
    __endNDXDebugInfo output_nop
.endmacro

; Start code profiling.
.macro ndxStartProfiling output_nop
    .ifndef DEBUG
        .exitmac
    .endif

    __startNDXDebugInfo __NDXDebugType::START_PROFILING
    __endNDXDebugInfo output_nop
.endmacro

; Stop code profiling.
.macro ndxStopProfiling output_nop
    .ifndef DEBUG
        .exitmac
    .endif

    __startNDXDebugInfo __NDXDebugType::STOP_PROFILING
    __endNDXDebugInfo output_nop
.endmacro

.endif ; !NDXDEBUG_H
