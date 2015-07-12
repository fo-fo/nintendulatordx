#ifndef DEBUGEXT_H
#define DEBUGEXT_H

#include "Config.h"

namespace DebugExt
{
    const int NSF_JSR_INIT = 0x3FED;
    const int NSF_JSR_PLAY = 0x3F27;

    const bool NEVER_SHORTEN_PRERENDER_SCANLINE = false;

    void UpdateSourceCodeView();
    void LoadDebugInfo();
    void Init();
    void SourceFileSelChanged();
    void WatchListSelChanged();

    int GetCurrentSourceCodeAddress();
    void HandleTimer(int addr);
    void HandleNSFRoutineJump(int addr);
    void InitDlg(HWND hwndDlg);
    void Reset();

    void HandleJSR();
    void HandleRTS();
    void HandleIRQ();
    void HandleNMI();
    void HandleRTI();
    void Unload();
    void InputUpdated();

    // is_dummy tells if the memory read was a dummy one made during
    // instruction decoding
    void MemoryReadEvent( int addr, bool isDummy );
    void MemoryCodeReadEvent( int addr, bool isDummy );
    void MemoryWriteEvent( int addr, int val );

    void AbsAddrDiagnostic( int addr );

    void HandleDebugOutput();
    void HandleProfilingStartEnd( int addr );
    void RunCycle();
    void HandleDebuggerBreak( int pc );
    void InvalidateMemoryAccessInfo();
    void BeforeExecOp();
    void ExecOp();
    void triggerLuaHook();
    void killLuaThread();

    void resetHard();
    int readMemory( int addr );
    void writeMemory( int addr, unsigned char data );

    void writeTo2007WhenRendering();
    void invalidBlackUsedInRendering();

    const int LUA_CD_CANVAS_W = Config::SCREEN_SIZE_X;
    const int LUA_CD_CANVAS_H = Config::SCREEN_SIZE_Y;
    extern unsigned char lua_canvas_r[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];
    extern unsigned char lua_canvas_g[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];
    extern unsigned char lua_canvas_b[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];
    extern unsigned char lua_canvas_a[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];

    extern bool expand_macros;
    extern bool randomizeMemory;
    extern bool memoryWarnings;
    extern bool ntscAspectRatio;
    extern bool palAspectRatio;
    extern bool maskSafeArea;
    extern bool ntscFilter;
}

#endif //!DEBUGEXT_H
