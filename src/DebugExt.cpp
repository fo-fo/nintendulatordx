// Nintendulator Debugger Extensions (NintendulatorDX)
// by thefox//aspekt 2010-20xx

// This source is released as is, it's pretty hacky/messy. If I
// understand GPL right my modifications are infected with GPL as well,
// given that it can't be considered a "work as a whole". Sorry about
// that. ;=)

// There's some testing/misc stuff in the code that's not exposed in the
// GUI or really related to debugging, but I decided to leave in anyways:
// - controller state capturing to a file
// - keeping track of function call stack for profiling information
// - loading of PowerPak save state mappers' save state files

// One small modification was made to dbginfo.c: __cdecl was added to
// strcmp function pointer type to the function type because Nintendulator
// VS project defaults the calling convention to __fastcall.

// TODO:
// - generate a code/data log
// - more diagnostic warnings
//   - if certain PPU regs are written to before warmup
// - use banking info for symbols
// - fix the bug in Nintendulator where it calculates movie length
//   by dividing by 60 instead of the correct frame rate (60.098...)
// - better memory viewer/editor
// - display symbols in trace view
// - warn user if file modification doesnt match the one in the dbgfile
// - handle code in RAM
// - keep all files open at all times, this way user can set breakpoints
//   by clicking in modules other than the one which is currently being debugged
// - removing of watch list entries
// - expression parsing for watches

// Notes about the NSF player (2011-03-15):
// 3FF6 is the trampoline JMP to init, 3FF9 to play
//    3FED is the JSR to init trampoline (3FF0 = return addr)
//    3F27 is the JSR to play trampoline (3F2A = return addr)

// Captures controller state during each NMI and writes it out when
// the ROM is unloaded.
//#define CAPTURE_CONTROLLER_NMI

// Captures controller state for each rendered frame, should not be used
// together with CAPTURE_CONTROLLER_NMI.
//#define CAPTURE_CONTROLLER_FRAME

// Enable keeping track of application call stack (JSR/RTS/IRQ/RTI..).
// Doesn't work well with most commercial applications, but could be
// useful for homebrew.
//#define ENABLE_CALL_STACK

// Write call stack based profiling data to a file when the ROM is
// unloaded.
//#define WRITE_PROFILING_DATA

// Load a PowerPak save state mappers' save state (currently not exposed
// in the GUI).
//#define LOAD_POWERPAK_SAVESTATE

// Makes a log of DAC writes and their cycle counts.
//#define LOG_DAC_WRITES

// A new version of writing profiling data, based on the scopes retrieved
// from debugging info.
#define WRITE_PROFILING_DATA_NEW

// Break in to debugger when absolute address is used where a zeropage
// address could have been used.
//#define BREAK_ON_ABS_ADDR_DIAGNOSTIC

// Emulate some of the PowerPak registers.
// #define POWERPAK_EMULATION

#include "stdafx.h"
#include <time.h>
#include <windowsx.h>
#include "Nintendulator.h"
#include "resource.h"
#include "MapperInterface.h"
#include "NES.h"
#include "APU.h"
#include "Debugger.h"
#include "CPU.h"
#include "PPU.h"
#include "GFX.h"
#include "Genie.h"
#include "States.h"
#include "Controllers.h"
#include "dbginfo/dbginfo.h"
#include "DebugExt.h"
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <fstream>
#include <list>
#include <limits>
#include <iomanip>
#include <cassert>
#include <random>
#include <array>
#include <stack>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "iup.h"
#include "iuplua.h"
#include "iupluacontrols.h"
#include "cd.h"
#include "cdlua.h"
#include "cdluaiup.h"
#undef min
#undef max

namespace Debugger {
    extern HWND    CPUWnd;
}

namespace DebugExt {

const int NUM_TIMERS = 16;
const int TIMER_UPDATE_RATE = 1000;
const int MAX_CALL_STACK_SIZE = 100;
const int MEM_ACCESS_INFO_SIZE = 0x10000;

enum StackEntryType
{
    setUNKNOWN, setJSR, setIRQ, setRESET
};

enum MemoryAccessState
{
    MEM_NOT_WRITTEN,
    MEM_WRITTEN,
    MEM_DIAGNOSTIC_SHOWN
};
typedef std::vector< MemoryAccessState > MemoryAccessInfo;

// std::string is ansi, std::wstring is unicode
// stdstring/stdstringstream is unicode/ansi depending on compile time flags
#ifdef UNICODE
    typedef std::wstring stdstring;
    typedef std::wstringstream stdstringstream;
#else
    typedef std::string stdstring;
    typedef std::stringstream stdstringstream;
#endif

// contents of a source file line by line
typedef std::vector<stdstring> SourceFileData;

// information for a source file
struct SourceFile {
    SourceFile() {}
    std::string filename;
    SourceFileData data;
    unsigned id;
};
// map from filename to SourceFile
typedef std::map<std::string, SourceFile> SourceFileMap;

struct ActiveLine
{
    ActiveLine( SourceFile* src = 0, unsigned lin = 0 ) : source( src ),
        line ( lin ) {}
    SourceFile* source;
    unsigned line;
};
typedef std::vector< ActiveLine > ActiveLines;

// list of watches, same as the listbox
struct WatchEntry {
    WatchEntry() : index(0) {}
    stdstring label;
    int index; // since there can be several WatchEntries for same label this indicates which one
    cc65_symboldata symboldata;
    char data_size; // 0 is default, otherwise can be 'b' or 'w'
};
typedef std::vector<WatchEntry> WatchVector;

struct DebugTimer {
    DebugTimer() : start(0), current(0), max(0), min(std::numeric_limits<unsigned long long>::max()),
        total(0), num_timings(0), last_timer_update(0), running(false) {}
    unsigned long long start;
    unsigned long long current;
    unsigned long long max;
    unsigned long long min;
    unsigned long long total;
    unsigned int num_timings;
    DWORD last_timer_update;
    std::string title;
    bool running;
};

struct CallStackItem
{
    CallStackItem( int addr = -1, std::string sym = "", unsigned long long tim = 0,
        StackEntryType entry_typ = setUNKNOWN )
        : address( addr ), symbol( sym ), time( tim ), entry_type( entry_typ ) {}
    int address;
    std::string symbol;
    unsigned long long time;
    StackEntryType entry_type;
};
typedef std::vector< CallStackItem > CallStack;

// Generic initialized value type.
template< typename T, T init_value = 0 >
class InitializedType
{
public:
    InitializedType() : v( init_value ) {}
    InitializedType( const T& iv ) : v( iv ) {}
    operator T& ()
    {
        return v;
    }
    operator const T& () const
    {
        return v;
    }
private:
    T v;
};
// unsigned long long that's initialized to zero
typedef InitializedType< unsigned long long > MyULongLong;
typedef std::map< std::string, MyULongLong > ProfilingData;

static cc65_dbginfo dbginfo;
static std::string current_srcfile;
static HFONT srcviewfont;
static SourceFileMap sourcefiles;
static WatchVector watches;
static WNDPROC origeditproc;
static DebugTimer timers[NUM_TIMERS];
static CallStack call_stack;
#ifdef ENABLE_CALL_STACK
static bool call_stack_enabled = true;
#else
static bool call_stack_enabled = false;
#endif
static bool abs_addr_diagnostic_shown = false;
static bool ppu2007WriteDiagnosticShown = false;
static bool invalidBlackDiagnosticShown = false;
static ProfilingData profiling_data;
static MemoryAccessInfo memory_access_info;
static ActiveLines active_lines;
#ifdef CAPTURE_CONTROLLER_NMI
    typedef std::vector< unsigned char > ControllerCapture;
    ControllerCapture controller_capture;
#endif
#ifdef LOG_DAC_WRITES
    std::ostringstream dac_log;
    int last_sample_read_pos;
#endif
#ifdef WRITE_PROFILING_DATA_NEW
// Big array (0x800 * 0x1000 * 4 bytes ~= 33 MB, RAM is cheap!)
static unsigned prg_rom_cycles[ MAX_PRGROM_SIZE ][ 0x1000 ];
static unsigned long global_cycles;
struct ScopeProfilingInfo
{
    std::string     name;
    unsigned long   cycles;
};

// Maps from scope_id to information about the scope.
typedef std::map< unsigned, ScopeProfilingInfo > ScopeProfilingInfos;
bool collect_profiling_data;
#endif

unsigned char lua_canvas_r[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];
unsigned char lua_canvas_g[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];
unsigned char lua_canvas_b[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];
unsigned char lua_canvas_a[ LUA_CD_CANVAS_W * LUA_CD_CANVAS_H ];

POINT mouse_pos;
BOOL mouse_btn_left;
BOOL mouse_btn_right;
BOOL mouse_btn_middle;
// Joypad bits (0..255), -1 = not plugged in.
const int NUM_JOYPADS = 2;
short joy_bits[ NUM_JOYPADS ] = { -1, -1 };

// Whether to step into macros when stepping code in the source view.
bool expand_macros = true;

// Whether to randomize memory on power-on (hard reset).
bool randomizeMemory;

// Whether to give warnings when uninitialized memory is read.
bool memoryWarnings;

// Whether to use NTSC aspect ratio (1.143:1) for window sizing.
bool ntscAspectRatio;
bool palAspectRatio;

// Whether to mask (darken) a safe area at the borders of the screen, to make
// it easier to find out whether a game places stuff too close to the borders.
bool maskSafeArea;

// Whether to enable blargg's NTSC filter. This causes ntscAspectRatio setting
// to be ignored, and palAspectRatio setting to be interpreted differently.
bool ntscFilter;

// Whether to log code/data accesses.
bool generateCodeDataLog = false;

const int KB = 1024;

// ---------------------------------------------------------------------------

// Code/data log for a single byte
struct CodeDataLog
{
    CodeDataLog() :
        covered( false ),
        isOpcode( false ),
        executed( false ),
        read( false ),
        written( false )
    {}

    // If true, byte has been referred to in some way. If false, status of the
    // byte is unknown.
    bool covered;

    // If true, byte has been used as the first byte of an instruction.
    bool isOpcode;

    // If true, byte has been used as part of an instruction (the opcode
    // or an operand).
    bool executed;

    bool read;
    bool written;
};

// Fairly big array :)
static CodeDataLog codeDataLog[MAX_PRGROM_SIZE][0x1000];
bool codeDataLogUsed;

// ---------------------------------------------------------------------------

enum NDXDebugType
{
    NDXDT_STRING,
    NDXDT_BREAK,
    NDXDT_START_PROFILING,
    NDXDT_END_PROFILING,
    NDXDT_LUA_EXEC_STR,
    NDXDT_LUA_EXEC_FILE,
};

typedef std::map< std::string, cc65_symboldata > SymbolMap;

class ExDbgObject
{
public:
    ExDbgObject( const SymbolMap& symbol_map );
    virtual ~ExDbgObject() {};
    long getPC();
    void triggerCheck();
    virtual void triggerLua() { };
protected:
    // PC and bank number where this event should be triggered.
    // The bank relative value can be calculated from pc_ by ANDing it with INTERNAL_BANK_SIZE.
    int pc_;
    int bank_;
    std::string source_filename_;
private:
    virtual void trigger() = 0;
};

class ExDbgBreak : public ExDbgObject
{
public:
    ExDbgBreak( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo );
    void trigger();
private:
};

class ExDbgString : public ExDbgObject
{
public:
    ExDbgString( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo );
    void trigger();
private:
    std::vector< unsigned char > debug_string_;
};

class ExDbgLuaExecStr : public ExDbgObject
{
public:
    ExDbgLuaExecStr( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo );
    void trigger();
    void triggerLua();
private:
    std::string code_string_;
};

class ExDbgLuaExecFile : public ExDbgObject
{
public:
    ExDbgLuaExecFile( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo );
    ~ExDbgLuaExecFile();
    void trigger();
    void triggerLua();
private:
    //std::string file_contents_;
    int func_ref_;
};

class ExDbgStartProfiling : public ExDbgObject
{
public:
    ExDbgStartProfiling( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo );
    void trigger();
private:
};

class ExDbgEndProfiling : public ExDbgObject
{
public:
    ExDbgEndProfiling( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo );
    void trigger();
private:
};

// Map from PC to a vector of debug objects related to that PC.
typedef std::vector< ExDbgObject* > ExDbgObjectVector;
typedef std::map< unsigned, ExDbgObjectVector > ExDbgObjects;
ExDbgObjects exdbg_objects;
const int INTERNAL_BANK_SIZE = 0x1000;

lua_State* L;
HANDLE lua_thread_handle = INVALID_HANDLE_VALUE;
HANDLE lua_trigger_event = INVALID_HANDLE_VALUE;
HANDLE lua_trigger_done_event = INVALID_HANDLE_VALUE;
HANDLE luaCallbackReturnEvent = INVALID_HANDLE_VALUE;
ExDbgObject* lua_trigger_event_object = NULL;
bool lua_thread_running = false;
int lua_after_frame_callback_ref = LUA_NOREF;

static std::stack< unsigned short > luaCallbackSavedPc;

const unsigned short kLuaJsrCallbackAddress = 0x4050;
// +3 because JSR takes 3 bytes
const unsigned short kLuaJsrCallbackReturn = kLuaJsrCallbackAddress + 3;

// Address that is injected as an JSR
unsigned short luaCallbackInjectAddress = 0;

bool luaReturnedFromCallback = false;

// Mersenne twister random number generator instance.
static std::mt19937 rng;

static LRESULT APIENTRY WatchEditSubclass( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
static void RebuildWatchList();
static void LoadTimerTitles();
static stdstring GetTimerTitle( int timer );
static void PushToCallStack( StackEntryType entry_type );
static void PopFromCallStack( StackEntryType required_entry_type );
static void FreeDebugInfo();
static bool CallStackPop();
static void LoadPowerPakSaveState( const std::string& filename );
static void MMC1Write( unsigned char val, unsigned char reg );
static void LoadSourceFile( const char *sourcefilename, unsigned line, unsigned source_id );
static void FreeExtendedDebugInfo();
static void LoadExtendedDebugInfo( const TCHAR* filename );
static void ProcessExtendedDebugInfo( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo );
static void CollectProfilingData();
static void HandleExDbgObjects();
static void DebugOutputFromPointer( const unsigned char* text_ptr );
static void registerLuaFunctions( lua_State* L );
static void initLua();
static void loadLuaLibs();
static DWORD WINAPI luaThread( void* param );
static void startLuaThread();
void killLuaThread();
static void triggerLuaHookLuaThread();
static stdstring fullPathFromFilename( const stdstring& filename );
static stdstring stripFilename( const stdstring& filename );
static std::string strToUpper( const std::string& in_str );
static void initializeRandom( void* memory, size_t memorySize, unsigned char mask = 0xFF, int forbid = -1 );
static CodeDataLog* getCodeDataLog( int addr );

stdstring GetSourceFileNameFromComboBox(unsigned index)
{
    int itemlen = SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_GETLBTEXTLEN, (WPARAM)index, (LPARAM)0);
    if(itemlen == -1) return _T("");
    TCHAR *buf = new TCHAR[itemlen + 1];
    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_GETLBTEXT, (WPARAM)index, (LPARAM)buf);
    stdstring srcfilestr(buf, buf + itemlen);
    delete[] buf;
    return srcfilestr;
}

// called by Debugger when code is stepped, etc
void UpdateSourceCodeView()
{
    if(!dbginfo) return;

    active_lines.clear();

    const cc65_spaninfo* spaninfo = cc65_span_byaddr( dbginfo, CPU::PC );

    if(spaninfo) {
        int      deepest_macro_count = -1;
        unsigned deepest_macro_line  = 0;
        unsigned top_level_line = 0;

        // First is parent scope id, second is the scope data.
        // In other words, can be used to retrieve the child scope of
        // a scope.
        typedef std::map< int, cc65_scopedata > ScopeRelations;
        ScopeRelations scope_relations;
        typedef std::vector< stdstring > SourceComboEntries;
        SourceComboEntries source_combo_entries;

        SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_RESETCONTENT, (WPARAM)0, (LPARAM)0);
        for(unsigned j = 0; j < spaninfo->count; ++j) {
            const cc65_spandata* spandata = &spaninfo->data[ j ];

            if ( spandata->scope_count )
            {
                const cc65_scopeinfo* scopeinfo = cc65_scope_byspan( dbginfo, spandata->span_id );
                if ( scopeinfo )
                {
                    if ( scopeinfo->count )
                    {
                        const cc65_scopedata* scopedata = &scopeinfo->data[ 0 ];
                        std::string scope_name( scopedata->scope_name );
                        stdstring compat( scope_name.begin(), scope_name.end() );
                        /*EI.DbgOut(_T("In span %d, found a scope: %s (id = %d, type = %d), parent = %d, count = %d"), spandata->span_id, compat.c_str(),
                            scopedata->scope_id, scopedata->scope_type, scopedata->parent_id, scopeinfo->count );*/

                        if ( scope_relations.find( scopedata->parent_id ) != scope_relations.end() )
                        {
                            //EI.DbgOut(_T("Warning: More than one scope has the same parent."));
                        }

                        scope_relations[ scopedata->parent_id ] = *scopedata;
                        // Need to make a copy of the name (remember to free it).
                        scope_relations[ scopedata->parent_id ].scope_name = _strdup( scopedata->scope_name );
                    }

                    cc65_free_scopeinfo( dbginfo, scopeinfo );
                }
            }

            if ( !spandata->line_count )
            {
                continue;
            }

            const cc65_lineinfo* lineinfo = cc65_line_byspan( dbginfo, spaninfo->data[ j ].span_id );

            for(unsigned i = 0; i < lineinfo->count; ++i ) {

                const cc65_linedata* linedata = &lineinfo->data[ i ];

                const cc65_segmentinfo* seginfo = cc65_segment_byid( dbginfo, spandata->segment_id );

                assert( seginfo->count );

                const cc65_segmentdata* segdata = &seginfo->data[ 0 ];
                if(!segdata->output_name) {
                    // FIXME: this might/will happen when code in RAM is run?
                    EI.DbgOut(_T("Warning: Line info output name not set"));
                    cc65_free_segmentinfo( dbginfo, seginfo );
                    continue;
                }

                int sub_size = 0;
                if(RI.ROMType == ROM_INES) {
                    sub_size = 16;
                } else if(RI.ROMType == ROM_NSF) {
                    sub_size = 128;
                }

                unsigned lineinfobank = (segdata->output_offs + spandata->span_start - segdata->segment_start - sub_size) / 4096;

                cc65_free_segmentinfo( dbginfo, seginfo );

                int curbank = -1;
                bool prgram_bank = false;
                unsigned pcbank = CPU::PC / 4096;
                // what bank is currently mapped at PC.. not set for RAM etc
                if(EI.GetPRG_ROM4(pcbank) >= 0) {
                    curbank = EI.GetPRG_ROM4(pcbank);
                } else if(EI.GetPRG_RAM4(pcbank) >= 0) {
                    curbank = EI.GetPRG_RAM4(pcbank);
                    prgram_bank = true;
                }

                // Always load all line infos for now.

                const cc65_sourceinfo* sourceinfo = cc65_source_byid( dbginfo, linedata->source_id );
                assert( sourceinfo->count );
                const cc65_sourcedata* sourcedata = &sourceinfo->data[ 0 ];

                if(lineinfobank == static_cast< unsigned int >( curbank )) {
                    //std::string ansifilename(lineinfo->data[i].source_name);
                    std::string ansifilename( sourcedata->source_name );
                    stdstring filename(ansifilename.begin(), ansifilename.end());
                    stdstringstream stdss;
                    stdss << filename << "(" << linedata->source_line << ")";
                    //SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_ADDSTRING, (WPARAM)0, (LPARAM)stdss.str().c_str());
                    source_combo_entries.push_back( stdss.str() );

                    // attempt to load the source file
                    LoadSourceFile( sourcedata->source_name, linedata->source_line, sourcedata->source_id );

                    if ( linedata->line_type == CC65_LINE_MACRO &&
                        int ( linedata->count ) > deepest_macro_count )
                    {
                        deepest_macro_count = linedata->count;
                        deepest_macro_line  = active_lines.size() - 1;
                    }
                    // If the line data is from C source, it always takes
                    // priority.
                    else if ( linedata->line_type == CC65_LINE_EXT )
                    {
                        deepest_macro_count = INT_MAX;
                        deepest_macro_line = active_lines.size() - 1;
                    }

                    if ( linedata->line_type == CC65_LINE_ASM || linedata->line_type == CC65_LINE_EXT )
                    {
                        top_level_line = active_lines.size() - 1;
                    }
                }

                cc65_free_sourceinfo( dbginfo, sourceinfo );
            }

            cc65_free_lineinfo(dbginfo, lineinfo);
        }

        // Figure out the current active scope, starting from the global scope.
        stdstring full_scope;
        {
            int cur = CC65_INV_ID;
            for ( ;; )
            {
                auto it = scope_relations.find( cur );
                if ( it == scope_relations.end() )
                {
                    break;
                }
                cc65_scopedata& sd = it->second;
                std::string scope_name( sd.scope_name );
                stdstring compat( scope_name.begin(), scope_name.end() );

                //EI.DbgOut(_T("Scope name = %s id = %d"), compat.c_str(), cur );

                if ( !compat.empty() )
                {
                    full_scope += _T( "::" ) + compat;
                }

                free( ( void* )sd.scope_name );

                cur = it->second.scope_id;
            }

            //EI.DbgOut(_T("Scope: %s"), full_scope.c_str() );
        }

        for ( auto it = source_combo_entries.begin(); it != source_combo_entries.end(); ++it )
        {
            stdstring all = ( *it ) + full_scope;
            SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_ADDSTRING, (WPARAM)0, (LPARAM)all.c_str());
        }

        {
            int selindex = 0;
            if ( expand_macros )
            {
                if ( deepest_macro_count != -1 )
                {
                    selindex = deepest_macro_line;
                }
            }
            else
            {
                selindex = top_level_line;
            }
            SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_SETCURSEL, (WPARAM)selindex/*selection*/, (LPARAM)0);
            SourceFileSelChanged();
        }

        cc65_free_spaninfo( dbginfo, spaninfo );
    }

    // update watch variables (read new values from memory)
    RebuildWatchList();
}

void DbgInfoError(const struct cc65_parseerror *err)
{
    std::string stderrstr(err->errormsg);
    stdstring errstr(stderrstr.begin(), stderrstr.end());

    EI.DbgOut(_T("Debug info %s on line %d: %s"), (err->type == CC65_WARNING ? _T("warning") : _T("error")),
        err->line, errstr.c_str());
}

stdstring cStringToTString( const char* c_str )
{
    std::string str( c_str );
    return stdstring( str.begin(), str.end() );
}

std::string tStringToCString( TCHAR* t_str )
{
    stdstring str( t_str );
    return std::string( str.begin(), str.end() );
}

std::string tStringToCString( const stdstring& t_str )
{
    return std::string( t_str.begin(), t_str.end() );
}

void LoadDebugInfo()
{
    if(!NES::ROMLoaded)
    {
        return;
    }

#ifdef LOAD_POWERPAK_SAVESTATE
    LoadPowerPakSaveState( "test.sav");
#endif

    if(dbginfo) FreeDebugInfo();
    FreeExtendedDebugInfo();

    if(RI.ROMType != ROM_INES && RI.ROMType != ROM_NSF) {
        EI.DbgOut(_T("Debug info not supported for this ROM type"));
        return;
    }

    stdstring dbgfilename( RI.Filename );
    dbgfilename += _T(".dbg");

    // TODO: what's the proper way to convert std::wstring to std::string?
    std::string dbgfilenameansi(dbgfilename.begin(), dbgfilename.end());

    dbginfo = cc65_read_dbginfo(dbgfilenameansi.c_str(), DbgInfoError);
    if(dbginfo) {
        EI.DbgOut(_T("Loaded debug info '%s'"), dbgfilename.c_str());
    } else {
        EI.DbgOut(_T("Couldn't load debug info '%s'"), dbgfilename.c_str());
    }

    current_srcfile = "";
    sourcefiles.clear();

    LoadTimerTitles();

    // Is it safe to assume the emulator thread isn't running at this point?

    // Lua thread can't be running while extended debug info is being processed, because
    // LoadExtendedDebugInfo needs to access Lua state.
    killLuaThread();
    initLua();
    // Has to come after Lua initialization.
    LoadExtendedDebugInfo( RI.Filename );
    startLuaThread();
}

void startLuaThread()
{
    // Create thread synchronization objects.
    // Default security attributes, auto-reset, default to non-signaled.
    lua_trigger_event = CreateEvent( NULL, FALSE, FALSE, NULL );
    lua_trigger_done_event = CreateEvent( NULL, FALSE, FALSE, NULL );
    luaCallbackReturnEvent = CreateEvent( NULL, FALSE, FALSE, NULL );

    DWORD lua_thread_id = 0;
    // Create a thread for Lua.
    lua_thread_running = true;
    lua_thread_handle = CreateThread( NULL, 0, luaThread, NULL, 0, &lua_thread_id );
}

void killLuaThread()
{
    lua_thread_running = false;
    if ( lua_thread_handle != INVALID_HANDLE_VALUE )
    {
        // Wait for it to die.
        WaitForSingleObject( lua_thread_handle, INFINITE );
        CloseHandle( lua_thread_handle );
        lua_thread_handle = INVALID_HANDLE_VALUE;
    }
    // Also delete the thread synchronization objects.
    CloseHandle( lua_trigger_event );
    CloseHandle( lua_trigger_done_event );
}

void doLuaTrigger()
{
    if ( lua_trigger_event_object )
    {
        // Got the trigger event. Execute it now.
        lua_trigger_event_object->triggerLua();
    }
    else
    {
        // Hacky... if lua_trigger_event_object is 0, call this.
        triggerLuaHookLuaThread();
    }
    // Let the emulator thread know that we're done here.
    SetEvent( lua_trigger_done_event );
}

DWORD WINAPI luaThread( void* param )
{
    loadLuaLibs();

    while ( lua_thread_running )
    {
        // Wait for a trigger event.
        const int TIMEOUT = 10; // ms
        if ( WaitForSingleObject( lua_trigger_event, TIMEOUT ) == WAIT_OBJECT_0 )
        {
            doLuaTrigger();
        }
        // Timeout (or got a trigger event), handle messages.
        IupLoopStep();
    }

    return 0;
}

void initLua()
{
    if ( L )
    {
        lua_close( L ); L = 0;
    }
    L = luaL_newstate();

    lua_after_frame_callback_ref = LUA_NOREF;
}

void loadLuaLibs()
{
    luaL_openlibs( L );
    iuplua_open( L );
    iupcontrolslua_open( L );
    cdlua_open( L );
    cdluaiup_open( L );
    registerLuaFunctions( L );
}

void stopEmulation()
{
    if ( NES::ROMLoaded )
        NES::DoStop = TRUE;
}

void FreeExtendedDebugInfo()
{
    for ( ExDbgObjects::iterator it = exdbg_objects.begin(); it != exdbg_objects.end(); ++it )
    {
        for ( ExDbgObjectVector::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2 )
        {
            delete *it2;
        }
    }

    exdbg_objects.clear();
}

void LoadExtendedDebugInfo( const TCHAR* filename )
{
    // Extended debug info is useless without cc65 debug info.
    if ( !dbginfo )
        return;

    // Load extended debug info, if available.
    stdstring exdbgfilename( filename );
    exdbgfilename += _T(".ndx");
    std::ifstream exifs( exdbgfilename, std::ios::binary );
    if ( exifs.is_open() )
    {
        exifs.seekg( 0, std::ios::end );
        size_t exifs_size = exifs.tellg();
        exifs.seekg( 0, std::ios::beg );
        std::vector< unsigned char > exdbginfo( exifs_size );
        if ( exifs_size )
        {
            exifs.read( ( char* ) &exdbginfo[ 0 ], exifs_size );
        }
        // Look for __NDXDebug scopes. Check the "type" symbol, and handle accordingly.
        const cc65_scopeinfo* scopes = cc65_scope_byname( dbginfo, "__NDXDebug" );
        if ( scopes )
        {
            for ( unsigned scope_index = 0; scope_index < scopes->count; ++scope_index )
            {
                const cc65_scopedata& scope_data = scopes->data[ scope_index ];
                if ( scope_data.scope_type == CC65_SCOPE_SCOPE )
                {
                    //EI.DbgOut(_T("Found an extended debug info scope"));
                    // Get the symbols.
                    SymbolMap symbol_map;
                    const cc65_symbolinfo* symbols = cc65_symbol_byscope( dbginfo, scope_data.scope_id );
                    if ( symbols )
                    {
                        // Load all of the symbols to a map for easy access.
                        for ( unsigned sym_index = 0; sym_index < symbols->count; ++sym_index )
                        {
                            const cc65_symboldata& symbol_data = symbols->data[ sym_index ];
                            symbol_map[ symbol_data.symbol_name ] = symbol_data;
                        }
                        for ( SymbolMap::iterator it = symbol_map.begin(); it != symbol_map.end(); ++it )
                        {
                            stdstring tname( it->first.begin(), it->first.end() );
                            //EI.DbgOut(_T("  Found a symbol %s: %d"), tname.c_str(), it->second.symbol_value);
                        }
                        // TODO: Pass the symbols (and exdbginfo) to another function which generates an object/whatever
                        // out of them based on type etc.
                        ProcessExtendedDebugInfo( symbol_map, exdbginfo );
                        // NOTE: The names in symbol map are probably not valid after this.
                        cc65_free_symbolinfo( dbginfo, symbols );
                    }
                }
            }
            cc65_free_scopeinfo( dbginfo, scopes );
        }
        EI.DbgOut(_T("Loaded extended debug info '%s'"), exdbgfilename.c_str());
    }
}

void ProcessExtendedDebugInfo( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo )
{
    SymbolMap::const_iterator type_iter = symbol_map.find( "type" );
    if ( type_iter == symbol_map.end() )
    {
        return;
    }
    NDXDebugType type = ( NDXDebugType )type_iter->second.symbol_value;
    ExDbgObject* obj = 0;
    switch ( type )
    {
    case NDXDT_BREAK:
        obj = new ExDbgBreak( symbol_map, exdbginfo );
        break;
    case NDXDT_STRING:
        obj = new ExDbgString( symbol_map, exdbginfo );
        break;
    case NDXDT_LUA_EXEC_STR:
        obj = new ExDbgLuaExecStr( symbol_map, exdbginfo );
        break;
    case NDXDT_LUA_EXEC_FILE:
        obj = new ExDbgLuaExecFile( symbol_map, exdbginfo );
        break;
    case NDXDT_START_PROFILING:
        obj = new ExDbgStartProfiling( symbol_map, exdbginfo );
        break;
    case NDXDT_END_PROFILING:
        obj = new ExDbgEndProfiling( symbol_map, exdbginfo );
        break;
    default:
        // Unknown type. Don't do anything for forward compatibility.
        ;
    }

    if ( obj )
    {
        exdbg_objects[ obj->getPC() ].push_back( obj );
    }
}

ExDbgObject::ExDbgObject( const SymbolMap& symbol_map ) :
    pc_( -1 ),
    bank_( -1 )
{
    //EI.DbgOut(_T("ExDbgObject object constructed"));

    // If symbol_map has a PC, construct pc_ and bank_ from it.
    SymbolMap::const_iterator pc_iter = symbol_map.find( "PC" );
    if ( pc_iter == symbol_map.end() )
    {
        return;
    }

    long pc = pc_iter->second.symbol_value;
    assert( pc >= 0 );

    // Figure out the bank from the output offset.
    const cc65_segmentinfo* segments = cc65_segment_byid( dbginfo, pc_iter->second.segment_id );
    if ( segments )
    {
        assert( segments->count == 1 );
        const cc65_segmentdata& segment_data = segments->data[ 0 ];

        // Subtract segment start from PC to get segment relative value, then add output offset
        // to get output file relative value.
        long output_relative = pc - segment_data.segment_start + segment_data.output_offs;
        //EI.DbgOut(_T("output_relative = %x"), output_relative);

        int header_size = -1;
        if ( RI.ROMType == ROM_INES )
        {
            header_size = 16;
        }
        else if ( RI.ROMType == ROM_NSF )
        {
            header_size = 128;
        }
        assert( header_size != -1 );

        // Calculate bank by subtracting header size and dividing by internal bank size.
        bank_   = ( output_relative - header_size ) / INTERNAL_BANK_SIZE;
        pc_     = pc;

        cc65_free_segmentinfo( dbginfo, segments );
    }

    // Get the scope from the symbol, then get the module name from that, and finally get
    // the source filename.
    const cc65_scopeinfo* scopes = cc65_scope_byid( dbginfo, pc_iter->second.scope_id );
    if ( scopes )
    {
        assert( scopes->count == 1 );
        const cc65_scopedata& scope_data = scopes->data[ 0 ];
        const cc65_moduleinfo* modules = cc65_module_byid( dbginfo, scope_data.module_id );
        if ( modules )
        {
            assert( modules->count == 1 );
            const cc65_moduledata& module_data = modules->data[ 0 ];

            const cc65_sourceinfo* sources = cc65_source_byid( dbginfo, module_data.source_id );
            if ( sources )
            {
                assert( sources->count == 1 );
                const cc65_sourcedata& source_data = sources->data[ 0 ];

                //EI.DbgOut(_T("ExDbgObject source filename: %s"), cStringToTString( source_data.source_name ).c_str());
                source_filename_ = source_data.source_name;

                cc65_free_sourceinfo( dbginfo, sources );
            }

            cc65_free_moduleinfo( dbginfo, modules );
        }
        cc65_free_scopeinfo( dbginfo, scopes );
    }

}

long ExDbgObject::getPC()
{
    return pc_;
}

void ExDbgObject::triggerCheck()
{
    // Check if the bank is correct. We already know the PC to be in range.

    const unsigned  pc_bank         = CPU::PC / 4096;
    const int       current_bank    = EI.GetPRG_ROM4( pc_bank );
    if ( current_bank >= 0 && bank_ == current_bank )
    {
        trigger();
    }
}

ExDbgBreak::ExDbgBreak( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo ) :
    ExDbgObject( symbol_map )
{
    //EI.DbgOut(_T("ExDbgBreak object constructed"));
}

void ExDbgBreak::trigger()
{
    //EI.DbgOut(_T("ExDbgBreak object triggered at %x"), CPU::PC);
    HandleDebuggerBreak( CPU::PC );
}

ExDbgString::ExDbgString( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo ) :
    ExDbgObject( symbol_map )
{
    //EI.DbgOut(_T("ExDbgString object constructed"));

    // string_start points to the string in exdbginfo, string_len has the length.
    auto start_iter = exdbginfo.begin() + symbol_map.at( "string_start" ).symbol_value;
    debug_string_ = std::vector< unsigned char >( start_iter, start_iter + symbol_map.at( "string_len" ).symbol_value );
    // For legacy, terminate with 0...
    debug_string_.push_back( 0 );
}

void ExDbgString::trigger()
{
    //EI.DbgOut(_T("ExDbgString object triggered at %x"), CPU::PC);

    DebugOutputFromPointer( &debug_string_[ 0 ] );
}

ExDbgLuaExecStr::ExDbgLuaExecStr( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo ) :
    ExDbgObject( symbol_map )
{
    //EI.DbgOut(_T("ExDbgLuaExecStr object constructed"));

    // string_start points to the string in exdbginfo, string_len has the length.
    auto start_iter = exdbginfo.begin() + symbol_map.at( "string_start" ).symbol_value;
    code_string_ = std::string( start_iter, start_iter + symbol_map.at( "string_len" ).symbol_value );
}

void ExDbgLuaExecStr::trigger()
{
    //EI.DbgOut(_T("ExDbgLuaExecStr object triggered at %x"), CPU::PC);

    // TODO! Use luaL_loadbuffer in the constructor so it doesn't have to be compiled every time.

    // We're currently in the emulation thread, Lua code needs to be executed from the Lua thread.
    // Set a global object and an event to let the Lua thread know it needs to execute something.
    // The actual meat of the function is in triggerLua().
    lua_trigger_event_object = this;
    SetEvent( lua_trigger_event );
    // Wait for the Lua chunk to finish executing.
    WaitForSingleObject( lua_trigger_done_event, INFINITE );
}

void ExDbgLuaExecStr::triggerLua()
{
    // Execute the string (code_string_).
    int error = luaL_dostring( L, code_string_.c_str() );
    if ( error )
    {
        EI.DbgOut( _T( "Lua error: %s" ), cStringToTString( lua_tostring( L, -1 ) ).c_str() );
        lua_pop( L, 1 );
        stopEmulation();
    }
    else
    {
        //EI.DbgOut( _T( "Lua code executed succesfully" ) );
    }
}

ExDbgLuaExecFile::ExDbgLuaExecFile( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo ) :
    ExDbgObject( symbol_map ),
    func_ref_( LUA_NOREF )
{
    //EI.DbgOut(_T("ExDbgLuaExecFile object constructed"));

    // string_start points to the string in exdbginfo, string_len has the length.
    auto start_iter = exdbginfo.begin() + symbol_map.at( "string_start" ).symbol_value;

    std::string filename = std::string( start_iter, start_iter + symbol_map.at( "string_len" ).symbol_value );

    // Get the path of the module, add that to the path of the ROM, and add the name of the Lua file to that.
    // For example, if ROM is in C:\foo\bar\zoo.nes, and module path is (relative to zoo.nes) src\something.s,
    // including car.lua will fetch it from C:\foo\bar\src\car.lua

    stdstring source_file_path_only = stripFilename( cStringToTString( source_filename_.c_str() ) );
    stdstring rom_path_only = fullPathFromFilename( RI.Filename );

    std::string full_path = tStringToCString( rom_path_only + source_file_path_only ) + filename;
    std::string relativePath = tStringToCString( source_file_path_only ) + filename;

    //EI.DbgOut( _T( "Full path is %s" ), cStringToTString( full_path.c_str() ).c_str() );
    
    // Compile the file and push it on the stack as a function.
    int err = luaL_loadfile( L, full_path.c_str() );
    if ( err )
    {
        // If load relative to ROM directory failed, try to open relative to
        // current directory.
        err = luaL_loadfile( L, relativePath.c_str() );
        if ( err )
            EI.DbgOut( _T( "Lua error (loadfile): %s" ), cStringToTString( lua_tostring( L, -1 ) ).c_str() );
    }
    
    if ( !err )
    {
        // Store the function in the Lua registry. This also pops it off the stack.
        func_ref_ = luaL_ref( L, LUA_REGISTRYINDEX );
        // Should never be nil because loadfile succeeded.
        assert( func_ref_ != LUA_REFNIL );

        EI.DbgOut( _T( "Loaded embedded Lua script '%s'" ), cStringToTString( relativePath.c_str() ).c_str() );
    }
}

void ExDbgLuaExecFile::trigger()
{
    //EI.DbgOut(_T("ExDbgLuaExecFile object triggered at %x"), CPU::PC);

    lua_trigger_event_object = this;
    SetEvent( lua_trigger_event );
    WaitForSingleObject( lua_trigger_done_event, INFINITE );
}

void ExDbgLuaExecFile::triggerLua()
{
    // Execute the compiled chunk using pcall.
    if ( func_ref_ != LUA_NOREF )
    {
        // Push the function on the stack.
        lua_rawgeti( L, LUA_REGISTRYINDEX, func_ref_ );
        // No parameters, no expected return values.
        const int num_args = 0;
        const int num_expected_results = LUA_MULTRET;
        const int error_handler = 0;
        int error = lua_pcall( L, num_args, num_expected_results, error_handler );
        if ( error )
        {
            EI.DbgOut( _T( "Lua error: %s" ), cStringToTString( lua_tostring( L, -1 ) ).c_str() );
            // Stop emulation on errors.
            stopEmulation();
        }
        else
        {
        }
    }
}

ExDbgLuaExecFile::~ExDbgLuaExecFile()
{
    if ( func_ref_ != LUA_NOREF )
    {
        // FIXME: This crashes the program
        // luaL_unref( L, LUA_REGISTRYINDEX, func_ref_ );
    }
}

ExDbgStartProfiling::ExDbgStartProfiling( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo ) :
    ExDbgObject( symbol_map )
{
    //EI.DbgOut(_T("ExDbgStartProfiling object constructed"));
}

void ExDbgStartProfiling::trigger()
{
    //EI.DbgOut(_T("ExDbgStartProfiling object triggered at %x"), CPU::PC);
    collect_profiling_data = true;
}

ExDbgEndProfiling::ExDbgEndProfiling( const SymbolMap& symbol_map, const std::vector< unsigned char >& exdbginfo ) :
    ExDbgObject( symbol_map )
{
    //EI.DbgOut(_T("ExDbgEndProfiling object constructed"));
}

void ExDbgEndProfiling::trigger()
{
    //EI.DbgOut(_T("ExDbgEndProfiling object triggered at %x"), CPU::PC);
    collect_profiling_data = false;
}

void LoadTimerTitles()
{
    if(!dbginfo) return;

    // search for symbols named "__timer1_title" etc
    for(int k = 0; k < NUM_TIMERS; ++k) {
        std::stringstream symbolss;
        symbolss << "__timer" << k << "_title";
        const cc65_symbolinfo *symbols = cc65_symbol_byname(dbginfo, symbolss.str().c_str());
        if(symbols) {
            for(unsigned i = 0; i < symbols->count; ++i) {
                const cc65_symboldata &sd = symbols->data[i];

                // Now uses segment info of symbol to retrieve, so the identifier string
                // in the beginning is not needed (or scanning through all banks)
                unsigned seg = sd.segment_id;
                const cc65_segmentinfo* seg_info = cc65_segment_byid( dbginfo, seg );

                assert( seg_info->count );

                int sub_size = 0;
                if(RI.ROMType == ROM_INES) {
                    sub_size = 16;
                } else if(RI.ROMType == ROM_NSF) {
                    sub_size = 128;
                }

                // This is the address of the title in the PRG data
                unsigned title_addr = sd.symbol_value - seg_info->data[0].segment_start + seg_info->data[0].output_offs - sub_size;
                timers[k].title.clear();
                unsigned char* base_ptr = NES::PRG_ROM[0];
                while ( base_ptr[ title_addr ] ) {
                    timers[k].title += base_ptr[title_addr];
                    ++title_addr;
                }
                stdstring titlecompat( timers[k].title.begin(),  timers[k].title.end());
                EI.DbgOut(_T("Found timer %d title: %s"), k, titlecompat.c_str());
            }
            cc65_free_symbolinfo(dbginfo, symbols);
        }
    }

}

void FreeDebugInfo()
{
    if(!dbginfo) return;

    cc65_free_dbginfo(dbginfo);
    dbginfo = 0;

    if ( L )
    {
        lua_close( L ); L = 0;
    }

    EI.DbgOut(_T("Debug info unloaded."));
}

void UnTabify(stdstring &str)
{
    stdstring::size_type off;
    while((off = str.find('\t')) != stdstring::npos) {
        str.replace(off, 1, _T("    "));
    }
}

void LoadSourceFile(const char *sourcefilename, unsigned line, unsigned source_id)
{
    // construct the complete path from RI.Filename and sourcefilename
    std::string srcfilestransi(sourcefilename);
    stdstring srcfilestr(srcfilestransi.begin(), srcfilestransi.end());

    auto it = sourcefiles.find(srcfilestransi);

    // If the source isn't loaded yet, load it.
    if(it == sourcefiles.end()) {
        SourceFile &srcfile = sourcefiles[srcfilestransi];

        srcfile.filename = srcfilestransi;
        srcfile.id = source_id;

        TCHAR rom_path[MAX_PATH];
        TCHAR *fname_ptr = NULL;
        GetFullPathName(RI.Filename, sizeof rom_path / sizeof rom_path[0], rom_path, &fname_ptr);
        *fname_ptr = 0;
        stdstring romfilename(rom_path);

        // Only append the ROM path if srcfilestr isn't a full path already.
        if ( !PathIsRelative( srcfilestr.c_str() ) )
        {
            romfilename.clear();
        }

        stdstring srcfilepath = romfilename + srcfilestr;

        std::wifstream ifs(srcfilepath);
        if(!ifs.is_open()) {
            // Try to open by using a path relative to current working directory.
            ifs.open( srcfilestr );
            if ( !ifs.is_open() )
            {
                EI.DbgOut( _T( "Couldn't load source file '%s'" ), srcfilepath.c_str() );
                return;
            }
        }

        int linenum = 1;
        while(!ifs.eof()) {
            stdstring line;
            std::getline(ifs, line);
            UnTabify(line);
            stdstringstream s;
            s << linenum << '\t' << line;
            srcfile.data.push_back(s.str());
            ++linenum;
        }

        EI.DbgOut(_T("Loaded source file '%s'"), srcfilepath.c_str());

        // Find the newly added one.
        it = sourcefiles.find(srcfilestransi);
    }

    // Add a line info with the specified line.
    active_lines.push_back( ActiveLine( &it->second, line ) );
}

void Init()
{
    if(!srcviewfont) {
        srcviewfont = CreateFont (14, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, ANSI_CHARSET, \
              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, \
              DEFAULT_PITCH | FF_SWISS, _T("Consolas"));
    }
    // Uncomment to use Consolas instead of the default font.
    //SendMessage(GetDlgItem(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST), WM_SETFONT, WPARAM (srcviewfont), TRUE);

    int watch_tabs[1] = { 90 };
    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_WATCH_LIST, LB_SETTABSTOPS, 1, (LPARAM)&watch_tabs);

    int timer_tabs[1] = { 80 };
    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_TIMER_LIST, LB_SETTABSTOPS, 1, (LPARAM)&timer_tabs);

    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_TIMER_LIST, LB_SETHORIZONTALEXTENT, 400, NULL);


    HWND hwnd_edit = GetDlgItem(Debugger::CPUWnd, IDC_DEBUG_WATCH_EDIT); 
    origeditproc = (WNDPROC)SetWindowLong(hwnd_edit, GWL_WNDPROC, (LONG)WatchEditSubclass);
}

void SourceFileSelChanged()
{
    unsigned srcfile_sel = SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_GETCURSEL, (WPARAM)0, (LPARAM)0);

    if ( srcfile_sel == CB_ERR || srcfile_sel >= active_lines.size() )
    {
        return;
    }

    // The selection corresponds to active_lines vector.
    ActiveLine& al = active_lines[ srcfile_sel ];

    std::string srcfilestransi = (al.source)->filename;
    SourceFile &srcfile = *al.source;

    if(srcfilestransi != current_srcfile) {
        SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, WM_SETREDRAW, FALSE, 0);
        SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, LB_RESETCONTENT, 0, 0);

        for(SourceFileData::iterator it = srcfile.data.begin(); it != srcfile.data.end(); ++it) {
            SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, LB_ADDSTRING, 0, (LPARAM)it->c_str());
        }    
    
        SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, WM_SETREDRAW, TRUE, 0);
        
        current_srcfile = srcfilestransi;
    }

    // set the selection in the sourcecode view (and scroll)
    const int num_lines_on_top = 8;
    int topline = al.line - num_lines_on_top - 1;
    if(topline < 0) topline = 0;
    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, LB_SETTOPINDEX, (WPARAM)topline, 0);

    // srcfile.line can be zero if no actual file was loaded
    if(al.line > 0) {
        SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, LB_SETCURSEL, (WPARAM)(al.line - 1), 0);
    } else {
        SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, LB_SETSEL, (WPARAM)FALSE, (LPARAM)-1);
    }
}

void WatchListSelChanged()
{
    HWND watchedit = GetDlgItem(Debugger::CPUWnd, IDC_DEBUG_WATCH_EDIT);

    int watchlist_sel = SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_WATCH_LIST, LB_GETCURSEL, (WPARAM)0, (LPARAM)0);
    if(watchlist_sel == LB_ERR) return;

    if(watches.size() < static_cast< unsigned int >( watchlist_sel )) return; // this would be pretty bad.. :"D
    // TODO: get the label from "watches"
    Edit_SetText(watchedit, watches[watchlist_sel].label.c_str());
    Edit_Enable(watchedit, TRUE);
}

void WatchEditEnterPressed()
{
    if(!dbginfo) return;

    bool doit = true;

    HWND watchedit = GetDlgItem(Debugger::CPUWnd, IDC_DEBUG_WATCH_EDIT);

    TCHAR buf[256];
    GetDlgItemText(Debugger::CPUWnd, IDC_DEBUG_WATCH_EDIT, buf, sizeof buf / sizeof buf[0]);
    Edit_SetText(watchedit, _T(""));

    stdstring label(buf);

    char data_size = 0;

    // check if user wants word sized data display
    size_t comma_position = label.find(',');
    if(comma_position != stdstring::npos) {
        if(comma_position == label.size() - 2) {
            // okay... next char should be data size
            data_size = label.back();
            if(data_size == 'b' || data_size == 'w') {
                label = label.substr(0, label.size() - 2);
            } else {
                // if erroneous data size indicator, don't add it
                doit = false;
                EI.DbgOut(_T("Erroneous data size indicator"));
            }
        } else {
            // don't add symbol if erroneous format (comma in wrong place, like "foo,fkkffk")
            doit = false;
        }
    }

    if(doit) {
        bool found = false;
        for(WatchVector::iterator it = watches.begin(); it != watches.end(); ++it) {
            if(it->label == buf) {
                found = true;
            }
        }
        if(!found) {
            std::string labelansi(label.begin(), label.end());

            stdstring bufstr(buf);
            const cc65_symbolinfo *symbols = cc65_symbol_byname(dbginfo, labelansi.c_str());
            if(symbols) {
                for(unsigned i = 0; i < symbols->count; ++i) {
                    const cc65_symboldata &sd = symbols->data[i];
                    // add to vector
                    WatchEntry we;
                    we.index = i;
                    we.label = bufstr;
                    we.symboldata = sd;
                    we.data_size = data_size;
                    // If user didn't specify a data size, use one from
                    // symbol data
                    if( !data_size )
                    {
                        switch( sd.symbol_size )
                        {
                        case 1:
                            we.data_size = 'b';
                            break;
                        case 2:
                            we.data_size = 'w';
                            break;
                        default:
                            we.data_size = 0;
                            break;
                        }
                    }
                    watches.push_back(we);
                }
            }

            RebuildWatchList();
            // TODO: make sure the new watch is visible...
        }
    }
}

void RebuildWatchList()
{
    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_WATCH_LIST, WM_SETREDRAW, FALSE, 0);
    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_WATCH_LIST, LB_RESETCONTENT, 0, 0);

    for(WatchVector::iterator it = watches.begin(); it != watches.end(); ++it) {
        cc65_symboldata &sd = it->symboldata;

        // Don't add imports to avoid duplicates, because a be a corresponding label/equate should always exist.
        if ( sd.symbol_type == CC65_SYM_IMPORT )
        {
            continue;
        }

        std::string ansilabel(sd.symbol_name);
        stdstring label(ansilabel.begin(), ansilabel.end());

        // type is either EQUATE or LABEL
        stdstringstream ss;
        if(it->index == 0) {
            ss << sd.symbol_name;
            if(it->data_size) {
                ss << ',' << it->data_size;
            }
        }
        ss << '\t' << "$" << std::hex << std::uppercase << sd.symbol_value;
        if(sd.symbol_type == CC65_SYM_EQUATE) ss << " (E)";
        else if(sd.symbol_type == CC65_SYM_LABEL) ss << " (L)";
        else if(sd.symbol_type == CC65_SYM_IMPORT) ss << " (I)";
        else ss << " (?)";
        int mem_val = 0;
        if(it->data_size == 'w') {
            mem_val = Debugger::DebugMemCPU(sd.symbol_value) | Debugger::DebugMemCPU((sd.symbol_value + 1) & 0xFFFF) << 8;
        } else {
            // default... (byte)
            mem_val = Debugger::DebugMemCPU(sd.symbol_value);
        }
        ss << " = $" << std::hex << mem_val << " (" << std::dec << mem_val << ")";

        SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_WATCH_LIST, LB_ADDSTRING, 0, (LPARAM)ss.str().c_str());
    }    

    SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_WATCH_LIST, WM_SETREDRAW, TRUE, 0);
}

LRESULT APIENTRY WatchEditSubclass(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) 
{
    if(uMsg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    } else if(uMsg == WM_KEYDOWN) {
        if(wParam == VK_RETURN) {
            WatchEditEnterPressed();
            return 0;
        }
    } else if(uMsg == WM_CHAR) {
        if(wParam == VK_RETURN) {
            return TRUE; // silence the bell
        }
    }

    return CallWindowProc(origeditproc, hwnd, uMsg, wParam, lParam); 
}

// get the address of the currently selected line in the source code view
int GetCurrentSourceCodeAddress()
{
    if(!dbginfo) return -1;

    // get current selection in source code listing
    int srclist_sel = SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCECODE_LIST, LB_GETCURSEL, (WPARAM)0, (LPARAM)0);
    if(srclist_sel == LB_ERR) return -1;

    // ...and in the source file selection combo box
    unsigned srcfile_sel = SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_SOURCEFILE, CB_GETCURSEL, (WPARAM)0, (LPARAM)0);
    if ( srcfile_sel == CB_ERR )
    {
        return -1;
    }

    const cc65_lineinfo* lineinfo = cc65_line_bynumber( dbginfo, active_lines[ srcfile_sel ].source->id, srclist_sel + 1 );

    int result = -1;

    if(lineinfo) {
        if(lineinfo->count) {
            // Just grab the first line info for now, with no regard to
            // the bank that's currently mapped in.
            const cc65_linedata* linedata = &lineinfo->data[ 0 ];
            // Get the corresponding span(s).
            const cc65_spaninfo* spaninfo = cc65_span_byline( dbginfo, linedata->line_id );
            if ( spaninfo )
            {
                if ( spaninfo->count )
                {
                    const cc65_spandata* spandata = &spaninfo->data[ 0 ];
                    // TODO: Is it ok to assume that the spans are sorted by
                    //   address?
                    result = spandata->span_start;
                }

                cc65_free_spaninfo( dbginfo, spaninfo );
            }
        }
        cc65_free_lineinfo(dbginfo, lineinfo);
    }

    return result;
}

unsigned long long PPUtoCPU(unsigned long long ppu_ticks)
{
    const double divider = PPU::IsPAL ? 3.2 : 3.0;

    return ppu_ticks / divider + 0.5;
}

void HandleDebugOutput()
{
    // Read string from PC+3 and output it.
    // This is a work in progress.

    unsigned pcbank = CPU::PC / 4096;
    // what bank is currently mapped at PC.. not set for RAM etc
    int curbank = EI.GetPRG_ROM4(pcbank);
    if( curbank < 0 )
    {
        // ... maybe code ran from RAM?
        return;
    }

    const unsigned char* text_ptr = NES::PRG_ROM[curbank] + CPU::PC % 4096 + 3;

    DebugOutputFromPointer( text_ptr );
}

void DebugOutputFromPointer( const unsigned char* text_ptr )
{
    std::ostringstream textss;
    while( *text_ptr )
    {
        char ch = *text_ptr++;

        switch( ch )
        {
        case 1:
            {
                // next byte indicates the format...
                int format_byte = *text_ptr++;

                int hex_or_dec = format_byte & 1; // 0 = hex, 1 = dec
                int val_size = format_byte >> 1;

                int addr = *(unsigned short*)text_ptr;
                text_ptr += 2;
                int mem_val = 0;
                // Number of characters to use.
                // 0 = no width set
                int hexWidth = 0;
                
                switch( val_size )
                {
                case 0:
                    {
                        mem_val = Debugger::DebugMemCPU(addr);
                        hexWidth = 2;
                        break;
                    }
                case 1:
                    {
                        mem_val = Debugger::DebugMemCPU(addr) |
                            Debugger::DebugMemCPU((addr + 1) & 0xFFFF) << 8;
                        hexWidth = 4;
                        break;
                    }
                }
                
                if( hex_or_dec == 0 )
                {
                    textss << std::hex << std::uppercase
                           << std::setfill( '0' ) << std::setw( hexWidth );
                }
                else
                {
                    textss << std::dec;
                }
                textss << mem_val;
                break;
            }
        default:
            {
                textss << ch;
                break;
            }
        }
    }

    std::string text = textss.str();
    stdstring textcompat( text.begin(), text.end() );
    EI.DbgOut(_T("Debug: %s"), textcompat.c_str());

}

void HandleTimer(int addr)
{
    // 401E/401F is a pair (virtuanes)
    // 402x/403x form 16 pairs for additional timers

    bool start = false;
    int timer = -1;

    if(addr == 0x401E) {
        timer = 0;
        start = true;
    } else if((addr & 0xFFF0) == 0x4020) {
        timer = addr & 0xF;
        start = true;
    } else if(addr == 0x401F) {
        timer = 0;
        start = false;
    } else if((addr & 0xFFF0) == 0x4030) {
        timer = addr & 0xF;
        start = false;
    }

    if(timer != -1) {
        for(int i = 0; i < NUM_TIMERS; ++i) {
            timers[i].start += 4;
        }

        if(start) {
            timers[timer].start = PPUtoCPU(PPU::TotalClockticks);
            timers[timer].start += 4;
            timers[timer].running = true;
        } else if(timers[timer].running) {
            timers[timer].start -= 4;

            timers[timer].running = false;

            unsigned long long diff = PPUtoCPU(PPU::TotalClockticks) - timers[timer].start;
            timers[timer].current = diff;
            if(diff > timers[timer].max) {
                timers[timer].max = diff;
            }
            if(diff < timers[timer].min) {
                timers[timer].min = diff;
            }
            timers[timer].num_timings++;
            timers[timer].total += diff;

            unsigned cur = diff;
            double avg = double(timers[timer].total) / timers[timer].num_timings;
            unsigned max = timers[timer].max;
            unsigned min = timers[timer].min;

            if(Debugger::CPUWnd && GetTickCount() - timers[timer].last_timer_update >= TIMER_UPDATE_RATE) {
                stdstringstream ss;
                ss << GetTimerTitle(timer) << "\tCur " << cur << " Avg " << avg << " Max " << max << " Min " << min;

                SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_TIMER_LIST, LB_DELETESTRING, timer, NULL);
                SendDlgItemMessage(Debugger::CPUWnd, IDC_DEBUG_TIMER_LIST, LB_INSERTSTRING, timer, (LPARAM)ss.str().c_str());

                timers[timer].last_timer_update = GetTickCount();
            }
            
        }

    }
}

stdstring GetTimerTitle(int timer)
{
    stdstringstream ss;
    if(!timers[timer].title.empty()) {
        stdstring titlecompat(timers[timer].title.begin(), timers[timer].title.end());
        ss << titlecompat;
    } else {
        ss << "Timer " << timer;
    }
    return ss.str();
}

void InitDlg(HWND hwndDlg)
{
    for(int i = 0; i < NUM_TIMERS; ++i) {
        stdstring timer_title = GetTimerTitle(i) + _T("\tinactive");
        SendDlgItemMessage(hwndDlg, IDC_DEBUG_TIMER_LIST, LB_ADDSTRING, 0, (LPARAM)timer_title.c_str());
    }

    CheckDlgButton(hwndDlg, IDC_EXPAND_MACROS, expand_macros ? BST_CHECKED : BST_UNCHECKED);
}

void StartTimer( int timer )
{
    HandleTimer( 0x4020 + timer );
}

void StopTimer( int timer )
{
    HandleTimer( 0x4030 + timer );
}

void HandleNSFRoutineJump( int addr )
{
    switch( addr )
    {
    case NSF_JSR_INIT:
        timers[ 14 ].title = "NSF Init";
        StartTimer( 14 );
        break;
    case NSF_JSR_INIT+3:
        StopTimer( 14 );
        break;
    case NSF_JSR_PLAY:
        timers[ 15 ].title = "NSF Play";
        StartTimer( 15 );
        break;
    case NSF_JSR_PLAY+3:
        StopTimer( 15 );
        break;
    }
}

void Reset()
{
    for(int i = 0; i < NUM_TIMERS; ++i)
    {
        std::string old_title = timers[i].title;
        timers[i] = DebugTimer();
        timers[i].title = old_title;
    }

    while( !call_stack.empty() )
    {
        call_stack.pop_back();
    }

    profiling_data.clear();

    PushToCallStack( setRESET );

    // NOTE: memory_access_info should not be cleared on soft reset!

    abs_addr_diagnostic_shown = false;
    ppu2007WriteDiagnosticShown = false;
    invalidBlackDiagnosticShown = false;

    watches.clear();
    RebuildWatchList();

#ifdef WRITE_PROFILING_DATA_NEW
    collect_profiling_data = false;
    memset( prg_rom_cycles, 0, sizeof prg_rom_cycles );
    global_cycles = 0;
#endif
}

void resetHard()
{
    // Called when a hard reset is triggered.

    if ( randomizeMemory )
    {
        // Initialize WRAM, CHR-RAM, RAM, VRAM, palette and OAM to random values.
        initializeRandom( (unsigned char *)NES::PRG_RAM + NES::SRAM_Size, sizeof( NES::PRG_RAM ) - NES::SRAM_Size );
        initializeRandom( NES::CHR_RAM, sizeof( NES::CHR_RAM ) );
        initializeRandom( CPU::RAM, sizeof( CPU::RAM ) );
        initializeRandom( PPU::VRAM, sizeof( PPU::VRAM ) );
        initializeRandom( PPU::Palette, sizeof( PPU::Palette ), 0x3F, 0xD );
        initializeRandom( PPU::Sprite, sizeof( PPU::Sprite ) );

        // If SRAM wasn't loaded from file, randomize it as well.
        if ( !NES::LoadedSRAMFromFile )
            initializeRandom( NES::PRG_RAM, NES::SRAM_Size );
    }

    // Set every memory location as "not accessed".
    memory_access_info.clear();
    memory_access_info.resize( MEM_ACCESS_INFO_SIZE, MEM_NOT_WRITTEN );

    // If SRAM was loaded from file, set the SRAM area as "written".
    if ( NES::LoadedSRAMFromFile )
    {
        for ( int i = 0x6000; i < 0x8000; ++i )
            memory_access_info.at( i ) = MEM_WRITTEN;
    }
}

void HandleJSR()
{
    PushToCallStack( setJSR );
}

void HandleRTS()
{
    PopFromCallStack( setJSR );
}

void HandleIRQ()
{
    PushToCallStack( setIRQ );
}

void HandleNMI()
{
    PushToCallStack( setIRQ );

#ifdef CAPTURE_CONTROLLER_NMI
    // strobe
    Controllers::Port1->Write( 1 );
    Controllers::Port1->Write( 0 );

    unsigned char ctl_state = 0;

    for( int i = 0; i < 8; ++i )
    {
        ctl_state = (ctl_state << 1) | Controllers::Port1->Read();
    }

#ifndef CAPTURE_CONTROLLER_FRAME
    controller_capture.push_back( ctl_state );
#endif
#endif
}

void HandleRTI()
{
    PopFromCallStack( setIRQ );
}

void PushToCallStack( StackEntryType entry_type )
{
    if( !call_stack_enabled )
    {
        return;
    }

    // Call stack is disabled if MAX_CALL_STACK_SIZE depth is reached,
    // this usually indicates that the app doesn't play nice with the stack.
    if( call_stack.size() >= MAX_CALL_STACK_SIZE )
    {
        EI.DbgOut(_T("Maximum call stack size reached -- disabling call stack"));
        call_stack_enabled = false;
        return;
    }

    std::string symbol_name;
    const cc65_symbolinfo* symbols = dbginfo ?
        cc65_symbol_inrange(dbginfo, CPU::PC, CPU::PC) : 0;
    if( symbols )
    {
        if( symbols->count > 1 )
        {
            EI.DbgOut(_T("More than one symbol found..."));
        }

        const cc65_symboldata& sd = symbols->data[ 0 ];
        symbol_name = sd.symbol_name;

        cc65_free_symbolinfo( dbginfo, symbols );
    }
    else
    {
        std::stringstream ss;
        ss << "unk_" << std::uppercase << std::hex << CPU::PC;
        symbol_name = ss.str();
    }

    call_stack.push_back( CallStackItem( CPU::PC, symbol_name, PPUtoCPU(PPU::TotalClockticks), entry_type ) );
}

void PopFromCallStack( StackEntryType required_entry_type )
{
    if( !call_stack_enabled )
    {
        return;
    }

    // record the elapsed cycles
    CallStackItem& top = call_stack.back();

    if( top.entry_type != required_entry_type )
    {
        // ops! show error (RTI was used to return after JSR, or RTS after IRQ)
        if( top.entry_type == setJSR )
        {
            EI.DbgOut(_T("RTI was used to return from JSR at %4X"), CPU::PC);
        }
        else if( top.entry_type == setIRQ )
        {
            EI.DbgOut(_T("RTS was used to return from IRQ at %4X"), CPU::PC);
        }

        call_stack_enabled = false;

        return;
    }

    CallStackPop();
}

bool CallStackPop()
{
    if( call_stack.empty() )
    {
        return false;
    }

    CallStackItem& top = call_stack.back();

    std::string full_name;

    // Build full name of the symbol.
    for( auto it = call_stack.begin(); it != call_stack.end(); ++it )
    {
        full_name += it->symbol + " -> ";
    }
    full_name.resize( full_name.size() - 4 );    

    unsigned long long current_time = PPUtoCPU(PPU::TotalClockticks);
    profiling_data[ full_name ] += current_time - top.time;

    // go down the stack
    call_stack.pop_back();

    return true;
}

#ifdef WRITE_PROFILING_DATA_NEW

void printProfilingInfoRecursively( const ScopeProfilingInfos&
    scope_profiling_infos, unsigned scope_id, std::ostream& ofs,
    int recursion = 0, unsigned long parent_cycles = 0 )
{
    const ScopeProfilingInfo& spi = scope_profiling_infos.at( scope_id );

    double cycles_perc = 100. * spi.cycles / parent_cycles;

    ofs << std::string( 2 * recursion, ' ' );
    ofs << spi.name << ": " << spi.cycles;
    
    if ( parent_cycles )
    {
        ofs << " (" << std::setprecision( 3 )
            << cycles_perc << " %)";
    }
    ofs << std::endl;

    const cc65_scopeinfo* child_scope_info = cc65_childscopes_byid( dbginfo,
        scope_id );

    if ( child_scope_info )
    {
        for ( unsigned i = 0; i < child_scope_info->count; ++i )
        {
            const cc65_scopedata& scope_data = child_scope_info->data[ i ];
            if ( scope_data.scope_type == CC65_SCOPE_SCOPE )
            {
                printProfilingInfoRecursively( scope_profiling_infos,
                    scope_data.scope_id, ofs, recursion + 1, spi.cycles );
            }
        }

        cc65_free_scopeinfo( dbginfo, child_scope_info );
    }
}

void writeCodeDataLog()
{
    if ( !codeDataLogUsed )
        return;

    stdstring codeDataLogFilename( RI.Filename );
    codeDataLogFilename += _T( ".cdl.txt" );
    std::ofstream ofs( codeDataLogFilename );

    const int bankSize = sizeof codeDataLog[0] / sizeof codeDataLog[0][0];
    const int numBanks = sizeof codeDataLog / sizeof codeDataLog[0];
    for ( int bank = 0; bank < numBanks; ++bank )
    {
        for ( int bankOffset = 0; bankOffset < bankSize; ++bankOffset )
        {
            CodeDataLog& cdl = codeDataLog[bank][bankOffset];
            if ( !cdl.covered )
                continue;

            int prgOffset = 4*KB * bank + bankOffset;

            ofs << std::hex << prgOffset << ' ';
            if ( cdl.executed )
                ofs << 'E';
            if ( cdl.isOpcode )
                ofs << 'O';
            if ( cdl.read )
                ofs << 'R';
            if ( cdl.written )
                ofs << 'W';
            ofs << '\n';
        }
    }
}

void writeNewProfilingData()
{
    if ( !dbginfo )
    {
        return;
    }

    // If nothing has been collected, don't write anything.
    if ( global_cycles == 0 )
    {
        return;
    }
    
    stdstring profilefilename( RI.Filename );
    profilefilename += _T( ".profiling.txt" );

    std::ofstream ofs( profilefilename );

    // Retrieve all scopes from the debugging information.
    const cc65_scopeinfo* scope_info = cc65_get_scopelist( dbginfo );

    if ( scope_info )
    {
        ScopeProfilingInfos scope_profiling_infos;

        // TODO: global_scope not currently exported properly by CC65...
        unsigned global_scope = UINT_MAX;

        // Since global_scope is not working, record all module scopes.
        std::vector< unsigned > module_scopes;

        // Go through all scopes. Calculate cycles for each scope, and also
        //   find the global scope.
        for ( unsigned i = 0; i < scope_info->count; ++i )
        {
            const cc65_scopedata& scope_data = scope_info->data[ i ];
            assert( scope_data.scope_id == i );
            // If a .scope/.proc...
            if ( scope_data.scope_type == CC65_SCOPE_SCOPE ||
                 scope_data.scope_type == CC65_SCOPE_GLOBAL ||
                 scope_data.scope_type == CC65_SCOPE_MODULE )
            {
                //ofs << "scope = " << scope_data.scope_name << std::endl;
                std::string module_name;
                if ( scope_data.scope_type == CC65_SCOPE_MODULE )
                {
                    const cc65_moduleinfo* module_info = cc65_module_byid(
                        dbginfo, scope_data.module_id );

                    const cc65_moduledata& module_data = module_info->data[ 0 ];
                    module_name = module_data.module_name;
                    //ofs << "  module name = " << module_data.module_name << std::endl;

                    cc65_free_moduleinfo( dbginfo, module_info );
                }

                // TODO: Save the information based on scope_id,
                // then when printing, start from the global scope and
                // print the info recursively.

                // Get the corresponding spans.
                const cc65_spaninfo* span_info = cc65_span_byscope( dbginfo,
                    scope_data.scope_id );
                if ( !span_info )
                {
                    continue;
                }

                unsigned long total_cycles = 0;

                for ( unsigned j = 0; j < span_info->count; ++j )
                {
                    const cc65_spandata& span_data = span_info->data[ j ];
                    // If span is not on the PRG-ROM area, don't count the cycles.
                    if ( span_data.span_start < 0x8000 )
                    {
                        continue;
                    }
                    //ofs << "  start  = " << span_data.span_start << std::endl;
                    // End is inclusive.
                    //ofs << "  end    = " << span_data.span_end << std::endl;
                    // Count cycles.
                    // Convert span start and end to file offsets.
                    //   - From span we can get the segment.
                    //   - From segment we can get the output offset.
                    const cc65_segmentinfo* seg_info = cc65_segment_byid( dbginfo, span_data.segment_id );
                    assert( seg_info->count == 1 );
                    const cc65_segmentdata& seg_data = seg_info->data[ 0 ];
                    unsigned span_start_file_offset = span_data.span_start - seg_data.segment_start + seg_data.output_offs;
                    unsigned span_end_file_offset   = span_data.span_end   - seg_data.segment_start + seg_data.output_offs;
                    cc65_free_segmentinfo( dbginfo, seg_info );
                    int sub_size = 0;
                    if(RI.ROMType == ROM_INES) {
                        sub_size = 16;
                    } else if(RI.ROMType == ROM_NSF) {
                        sub_size = 128;
                    }
                    for ( unsigned k = span_start_file_offset; k <= span_end_file_offset; ++k )
                    {
                        const unsigned prg_offset = k - sub_size;
                        const unsigned bank = prg_offset / 4096;
                        const unsigned bank_offset = prg_offset % 4096;
                        total_cycles += prg_rom_cycles[ bank ][ bank_offset ];
                    }
                }

                //ofs << "  cycles = " << total_cycles << std::endl;

                ScopeProfilingInfo scope_profiling_info;
                if ( scope_data.scope_type == CC65_SCOPE_SCOPE )
                {
                    scope_profiling_info.name       = scope_data.scope_name;
                }
                else if ( scope_data.scope_type == CC65_SCOPE_GLOBAL )
                {
                    //scope_profiling_info.name = "[global scope]";
                    assert( false );
                }
                else if ( scope_data.scope_type == CC65_SCOPE_MODULE )
                {
                    scope_profiling_info.name = "[module " + module_name + "]";
                }
                scope_profiling_info.cycles     = total_cycles;

                scope_profiling_infos[ scope_data.scope_id ] =
                    scope_profiling_info;

                if ( scope_data.scope_type == CC65_SCOPE_MODULE )
                {
                    module_scopes.push_back( scope_data.scope_id );
                }

                if ( scope_data.scope_type == CC65_SCOPE_GLOBAL )
                {
                    global_scope = scope_data.scope_id;
                }

                cc65_free_spaninfo( dbginfo, span_info );
            }
            else
            {
                /*
                ofs << "scope id = " << scope_data.scope_id << " name = "
                    << scope_data.scope_name << std::endl;
                ofs << "  type = " << scope_data.scope_type << std::endl;
                ofs << "  parent  = " << scope_data.parent_id << std::endl;
                */
            }
        }

        cc65_free_scopeinfo( dbginfo, scope_info );

        //assert( global_scope != UINT_MAX );

        for ( auto it = module_scopes.begin(); it != module_scopes.end(); ++it )
        {
            printProfilingInfoRecursively( scope_profiling_infos, *it,
                ofs, 0, global_cycles );
            ofs << std::endl;
        }
    }
}
#endif

void Unload()
{
    if( !NES::ROMLoaded )
    {
        return;
    }

#ifdef WRITE_PROFILING_DATA
    if( call_stack_enabled )
    {
        // pop everything off the call stack and apply the amounts
        while( CallStackPop() );

        stdstring profilefilename( RI.Filename );
        profilefilename += _T(".profiling.txt");

        std::ofstream ofs(profilefilename);

        for( auto it = profiling_data.begin(); it != profiling_data.end();
            ++it )
        {
            ofs << it->first << " " << it->second << std::endl;
        }
    }
#endif

#ifdef CAPTURE_CONTROLLER_NMI
    {
        stdstring keysfilename( RI.Filename );
        keysfilename += _T(".keys.txt");

        std::ofstream ofs(keysfilename);

        for( auto it = controller_capture.begin(); it !=
            controller_capture.end(); ++it )
        {
            // order is RLDUTSBA (T = Start, S = Select)
            ofs << "|0|";
            unsigned char ctl_state = *it;
            static const char* lut = "RLDUTSBA";
            for( int i = 0; i < 8; ++i )
            {
                ofs << ( ( ctl_state & 1 ) ? lut[ i ] : '.' );
                ctl_state >>= 1;
            }
            ofs << "|........||" << std::endl;
        }
    }

    {
        stdstring keysbinfilename( RI.Filename );
        keysbinfilename += _T(".keys.bin");

        std::ofstream ofs(keysbinfilename, std::ios::binary);
        if( !controller_capture.empty() )
        {
            ofs.write( (char*)&controller_capture[0], controller_capture.size() );
        }

    }
#endif

#ifdef LOG_DAC_WRITES

    {
        stdstring daclogfilename( RI.Filename );
        daclogfilename += _T(".dac.txt");

        std::ofstream ofs(daclogfilename, std::ios::binary);

        ofs << dac_log.str();
    }
#endif

#ifdef WRITE_PROFILING_DATA_NEW
    writeNewProfilingData();
#endif

    writeCodeDataLog();

    FreeDebugInfo();
}

void InputUpdated()
{
#ifdef CAPTURE_CONTROLLER_FRAME
    // strobe
    Controllers::Port1->Write( 1 );
    Controllers::Port1->Write( 0 );

    unsigned char ctl_state = 0;

    for( int i = 0; i < 8; ++i )
    {
        ctl_state = (ctl_state << 1) | Controllers::Port1->Read();
    }

    controller_capture.push_back( ctl_state );
#endif

    // Make a copy of the mouse state for comfort.
    GFX::GetCursorPos( &mouse_pos );
    const int DEV_MOUSE = 0x10000;
    mouse_btn_left    = Controllers::IsPressed( DEV_MOUSE | 0 );
    mouse_btn_right   = Controllers::IsPressed( DEV_MOUSE | 1 );
    mouse_btn_middle  = Controllers::IsPressed( DEV_MOUSE | 2 );

    //EI.DbgOut(_T("Mouse state: x = %d y = %d, buttons %d %d %d"), mouse_pos.x, mouse_pos.y, mouse_btn_left, mouse_btn_middle, mouse_btn_right );

    // Capture joypads.
    if ( Controllers::Port1->Type == Controllers::STD_STDCONTROLLER )
    {
        auto controller = (Controllers::StdPort_StdController *)Controllers::Port1;
        joy_bits[ 0 ] = controller->State->NewBits;
    }
    else
    {
        joy_bits[ 0 ] = -1;
    }

    if ( Controllers::Port2->Type == Controllers::STD_STDCONTROLLER )
    {
        auto controller = (Controllers::StdPort_StdController *)Controllers::Port2;
        joy_bits[ 1 ] = controller->State->NewBits;
    }
    else
    {
        joy_bits[ 1 ] = -1;
    }

    //EI.DbgOut(_T("Joy state: port1 = %02X port2 = %02X"), joy_bits[ 0 ], joy_bits[ 1 ] );
}

#ifdef LOAD_POWERPAK_SAVESTATE
void LoadPowerPakSaveState( const std::string& filename )
{
    std::ifstream ifs( filename, std::ios::binary );

    if( !ifs.is_open() )
    {
        EI.DbgOut(_T("Error: Couldn't open PowerPak save state!") );
        return;
    }

    // Skip the first 8K (game's WRAM)
    ifs.seekg( 0x2000 );

    ifs.read( ( char* )CPU::RAM, 0x800 ); // 2K of CPU RAM
    char nt_data[ 0x800 ];
    ifs.read( ( char* )nt_data, sizeof nt_data ); // 2K of nametables

    {
        // TODO: Other mirrorings, code below is for vertical.

        unsigned char* p = PPU::VRAM[0];
        int j = 0;
        for( int i = 0; i < 256; ++i )
        {
            *p++ = nt_data[ 0x400 * j + i ];
            *p++ = nt_data[ 0x400 * j + 0x100 + i ];
            *p++ = nt_data[ 0x400 * j + 0x200 + i ];
            *p++ = nt_data[ 0x400 * j + 0x300 + i ];
        }

        p = PPU::VRAM[1];
        j = 1;
        for( int i = 0; i < 256; ++i )
        {
            *p++ = nt_data[ 0x400 * j + i ];
            *p++ = nt_data[ 0x400 * j + 0x100 + i ];
            *p++ = nt_data[ 0x400 * j + 0x200 + i ];
            *p++ = nt_data[ 0x400 * j + 0x300 + i ];
        }
    }

    CPU::A = ifs.get();
    CPU::X = ifs.get();
    CPU::Y = ifs.get();
    CPU::SP = ifs.get();
    CPU::P = ifs.get();
    CPU::SplitFlags();

    const int PALETTE_SIZE = 32;
    ifs.read( ( char* )PPU::Palette, PALETTE_SIZE );
    ifs.get(); // Mirroring
    PPU::Reg2000 = ifs.get();
    PPU::Reg2001 = ifs.get();
    ifs.get(); // Unused (old one byte mapper state)

    // TODO: Check the signature BEFORE doing anything else...

    const int SIG_SIZE = 4;
    char sig[ SIG_SIZE ] = { };
    ifs.read( sig, SIG_SIZE );

    if( memcmp( sig, "meGA", SIG_SIZE ) != 0 )
    {
        EI.DbgOut(_T("Error: PowerPak save state signature didn't match!") );
    }

    // 16 bytes of mapper state (TODO)
    ifs.seekg( 0x3110 );

    // Assume MMC1
    // TODO: Other mappers
    MMC1Write( ifs.get(), 0 );
    MMC1Write( ifs.get(), 1 );
    MMC1Write( ifs.get(), 2 );
    MMC1Write( ifs.get(), 3 );

    ifs.seekg( 0x3200 );
    ifs.read( ( char* )PPU::Sprite, 0x100 ); // 256 bytes of OAM data

    ifs.seekg( 0x4000 );
    ifs.read( ( char* )NES::CHR_RAM, 0x2000 ); // 8K of CHR-RAM

    ifs.read( ( char* )NES::PRG_RAM, 0x2000 ); // 8K of WRAM

    // Start executing from the NMI
    CPU::PCL = CPU::MemGet(0xFFFA);
    CPU::PCH = CPU::MemGet(0xFFFB);
}

void MMC1Write( unsigned char val, unsigned char reg )
{
    int addr = 0x8000 + 0x2000 * reg;
    for( int i = 0; i < 5; ++i )
    {
        CPU::MemSet( addr, val );
        val >>= 1;
    }
}
#endif

void MemoryReadEvent( int addr, bool isDummy )
{
#ifdef LOG_DAC_WRITES
    if ( addr >= 0x200 && addr < 0x200 + 156 )
    {
        last_sample_read_pos = addr;
    }
#endif

    if ( !isDummy )
    {
        CodeDataLog* cdl = getCodeDataLog( addr );
        if ( cdl )
            cdl->read = true;
    }

    // Can happen because Nintendulator calls MemGet in the reset code
    if( memory_access_info.empty() )
    {
        return;
    }

    // Ignore dummy reads that happen during instruction decoding
    if( isDummy )
    {
        return;
    }

    // Don't display if the UI option is not set.
    if ( !memoryWarnings )
        return;

    // Internal RAM and WRAM (TODO: Handle mirrors)
    if( ( addr >= 0 && addr < 0x800 ) || ( addr >= 0x6000 && addr < 0x8000 ) )
    {
        // If reading, but have not written, give diagnostic
        if( memory_access_info[ addr ] == MEM_NOT_WRITTEN )
        {
            EI.DbgOut(_T("Warning: Uninitialized memory accessed: $%X (PC = $%X)"), addr, CPU::OpAddr);
            memory_access_info[ addr ] = MEM_DIAGNOSTIC_SHOWN;
        }
    }
}

void MemoryCodeReadEvent( int addr, bool isDummy )
{
    // Memory read happened as an instruction opcode/operand fetch.

    if ( !isDummy )
    {
        CodeDataLog* cdl = getCodeDataLog( addr );
        if ( cdl )
            cdl->executed = true;
    }
}

#ifdef LOG_DAC_WRITES
void LogDACWrite( int val )
{
    static long long previous_clocks = -1;

    long long current_clocks = PPUtoCPU( PPU::TotalClockticks );

    const int sample_num = last_sample_read_pos - 0x200;

    long long diff = 0;
    if ( previous_clocks != -1 )
    {
        diff = current_clocks - previous_clocks;

        // Modify diff to indicate by how many cycles it is off,
        // assuming samples are played every 213 cycles, and every 8th
        // sample takes 214 cycles...
        int correct_diff = sample_num % 8 != 7 ? 213 : 214;
        diff = correct_diff - diff;
    }

    previous_clocks = current_clocks;

    dac_log << std::dec << current_clocks << " (" << diff << "): " <<
        //std::hex << std::uppercase << std::setw( 2 ) << val <<
        sample_num <<
        std::endl;
}
#endif

void MemoryWriteEvent( int addr, int val )
{
    CodeDataLog* cdl = getCodeDataLog( addr );
    if ( cdl )
        cdl->written = true;

    memory_access_info[ addr ] = MEM_WRITTEN;

#ifdef LOG_DAC_WRITES
    if ( addr == 0x4011 )
    {
        LogDACWrite( val );
    }
#endif
}

void AbsAddrDiagnostic( int addr )
{
    // Display a diagnostic message if absolute addressing is used
    // where zeropage addressing could be used.

    // TODO: Also do the same for ABS,X etc
    // TODO: Keep track of all addresses, and display them somewhere which
    //   has no problem handling a lot of entries (like listbox)

#ifdef BREAK_ON_ABS_ADDR_DIAGNOSTIC
    if ( NES::ROMLoaded )
    {
        EI.DbgOut(_T("Info: Breaking at absolute address diagnostic at PC = $%X"), CPU::OpAddr);
        NES::DoStop = TRUE;
    }
#endif

    if( abs_addr_diagnostic_shown )
    {
        return;
    }

    EI.DbgOut(_T("Info: Absolute addressing used where zeropage could have been used: $%X (PC = $%X)"), addr, CPU::OpAddr);
    EI.DbgOut(_T("  (further messages suppressed)"));
    abs_addr_diagnostic_shown = true;
}

void RunCycle()
{
#ifdef WRITE_PROFILING_DATA_NEW
    CollectProfilingData();
#endif
}

CodeDataLog* getCodeDataLog( int addr )
{
    if ( !generateCodeDataLog )
        return nullptr;

    // 4 KB bank number
    unsigned pcBank = addr / ( 4 * KB );

    // Get the PRG ROM bank mapped in at the address.
    // \todo Make sure this works OK for code executing from non-PRG
    //       addresses (= returns -1).
    int bank = EI.GetPRG_ROM4( pcBank );
    if ( bank >= 0 )
    {
        // Offset in bank
        int bankOffset = addr % ( 4 * KB );

        // Get the CDL.
        CodeDataLog& cdl = codeDataLog[bank][bankOffset];

        // Always mark as covered.
        cdl.covered = true;

        codeDataLogUsed = true;

        return &cdl;
    }

    return nullptr;
}

void BeforeExecOp()
{
    // Don't handle ExDbgObjects again if we returned from a 6502 callback
    // to the same address again.
    if ( !luaReturnedFromCallback )
        HandleExDbgObjects();

    luaReturnedFromCallback = false;
}

void ExecOp()
{
    // Called after a opcode has been executed. CPU::OpAddr is the address of
    // the opcode.

    if ( !luaCallbackSavedPc.empty() )
    {
        if ( CPU::PC == kLuaJsrCallbackReturn )
        {
            // We're done with the callback, let the Lua thread continue
            // and wait for the original Lua execution to finish.
            // Restore the PC from before the callback.
            CPU::PC = luaCallbackSavedPc.top();
            luaCallbackSavedPc.pop();
            // \todo Not sure if this thing works correctly.
            luaReturnedFromCallback = true;
            SetEvent( luaCallbackReturnEvent );
            WaitForSingleObject( lua_trigger_done_event, INFINITE );
        }
    }

    CodeDataLog* cdl = getCodeDataLog( CPU::OpAddr );
    if ( cdl )
        cdl->isOpcode = true;
}

void HandleExDbgObjects()
{
    // If PC is at one of the extended debug info positions, process them.
    ExDbgObjects::iterator it = exdbg_objects.find( CPU::PC );
    if ( it == exdbg_objects.end() )
    {
        return;
    }

    ExDbgObjectVector& objects = it->second;
    for ( ExDbgObjectVector::iterator it = objects.begin(); it != objects.end(); ++it )
    {
        ( *it )->triggerCheck();
    }
}

void CollectProfilingData()
{
    if ( collect_profiling_data )
    {
        // Count cycles for this PC...
        // Actually we're counting all the cycles of an instruction for
        // the address of the opcode (CPU::OpAddr), but doesn't matter for our purposes.
        // This is because CPU::PC has already been increased at this
        // point.

        // Don't count from RAM etc.
        if ( CPU::OpAddr < 0x8000 )
        {
            return;
        }

        // Find out the bank of the current address.
        int cur_bank = -1;
        bool prgram_bank = false;
        unsigned pc_bank = CPU::OpAddr / 4096;
        // what bank is currently mapped at PC.. not set for RAM etc
        if(EI.GetPRG_ROM4(pc_bank) >= 0) {
            cur_bank = EI.GetPRG_ROM4(pc_bank);
        } else if(EI.GetPRG_RAM4(pc_bank) >= 0) {
            cur_bank = EI.GetPRG_RAM4(pc_bank);
            prgram_bank = true;
        }

        if ( cur_bank != -1 && !prgram_bank )
        {
            ++prg_rom_cycles[ cur_bank ][ CPU::OpAddr % 4096 ];
            
            // Count globally as well.
            ++global_cycles;
        }
        else
        {
            // This can happen when code is executed from RAM...
        }
    }
}

void HandleProfilingStartEnd( int addr )
{
#ifdef WRITE_PROFILING_DATA_NEW
    collect_profiling_data = addr == 0x4041;
#endif
}

void HandleDebuggerBreak( int pc )
{
    if ( NES::ROMLoaded )
    {
        EI.DbgOut(_T("Info: Debug breakpoint reached at PC = $%X"), pc);
        NES::DoStop = TRUE;
    }
}

void InvalidateMemoryAccessInfo()
{
    // Make all memory appear as initialized.
    for ( MemoryAccessInfo::iterator it = memory_access_info.begin(); it != memory_access_info.end(); ++it )
    {
        *it = MEM_DIAGNOSTIC_SHOWN;
    }
}

// Called from PPU when a particular scanline/cycle is hit.
// Use PPU::SLnum, PPU::Clockticks (and NES::Scanline) to find out which.
void triggerLuaHook()
{
    // Possible trigger locations:
    // - At the end of rendering (SLnum has just rolled over to 240, Clockticks has been reset to 0).
    //   PPU::SLnum == 240, PPU::Clockticks == 0, NES::Scanline = TRUE
    if ( PPU::SLnum == 240 && PPU::Clockticks == 0 )
    {
        // If a callback has been set, call it (using lua_pcall).
        if ( lua_after_frame_callback_ref != LUA_NOREF )
        {
            lua_trigger_event_object = NULL;
            SetEvent( lua_trigger_event );
            // luaThread will call triggerLuaHookLuaThread and then set lua_trigger_done_event.
            // Wait for the Lua chunk to finish executing.
            WaitForSingleObject( lua_trigger_done_event, INFINITE );
        }
        else
        {
            // Clear the canvas if no callback.
            memset( lua_canvas_a, 0, LUA_CD_CANVAS_W * LUA_CD_CANVAS_H );
        }
    }
}

void triggerLuaHookLuaThread()
{
    bool got_custom_canvas = false;

    // Push the function on the stack.
    lua_rawgeti( L, LUA_REGISTRYINDEX, lua_after_frame_callback_ref );
    // No parameters.
    const int num_args = 0;
    // One return value can be given (a canvas that should be drawn).
    const int num_expected_results = 1;
    const int error_handler = 0;
    int error = lua_pcall( L, num_args, num_expected_results, error_handler );
    if ( error )
    {
        EI.DbgOut( _T( "Lua error (pcall): %s" ), cStringToTString( lua_tostring( L, -1 ) ).c_str() );
    }
    else
    {
        if ( !lua_isnil( L, -1 ) )
        {
            cdCanvas* canvas = cdlua_checkcanvas( L, -1 );
            int num_color_planes = cdCanvasGetColorPlanes( canvas );
            int width, height;
            cdCanvasGetSize( canvas, &width, &height, NULL, NULL );
            // TODO: If bit depth is 24, just set the alpha channel to $FF.
            if ( num_color_planes == 32 && width == LUA_CD_CANVAS_W && height == LUA_CD_CANVAS_H )
            {
                unsigned char* red   = ( unsigned char* )cdCanvasGetAttribute( canvas, "REDIMAGE" );
                unsigned char* green = ( unsigned char* )cdCanvasGetAttribute( canvas, "GREENIMAGE" );
                unsigned char* blue  = ( unsigned char* )cdCanvasGetAttribute( canvas, "BLUEIMAGE" );
                unsigned char* alpha = ( unsigned char* )cdCanvasGetAttribute( canvas, "ALPHAIMAGE" );
                memcpy( lua_canvas_r, red,   LUA_CD_CANVAS_W * LUA_CD_CANVAS_H );
                memcpy( lua_canvas_g, green, LUA_CD_CANVAS_W * LUA_CD_CANVAS_H );
                memcpy( lua_canvas_b, blue,  LUA_CD_CANVAS_W * LUA_CD_CANVAS_H );
                memcpy( lua_canvas_a, alpha, LUA_CD_CANVAS_W * LUA_CD_CANVAS_H );
                got_custom_canvas = true;
            }
            else
            {
                EI.DbgOut( _T( "Invalid canvas dimensions (should be %dx%dx32)." ), LUA_CD_CANVAS_W, LUA_CD_CANVAS_H );
            }
        }
    }

    if ( !got_custom_canvas )
    {
        memset( lua_canvas_a, 0, LUA_CD_CANVAS_W * LUA_CD_CANVAS_H );
    }
}

std::string strToUpper( const std::string& in_str )
{
    std::string result;
    for ( auto it = in_str.begin(); it != in_str.end(); ++it )
    {
        result.push_back( toupper( *it ) );
    }
    return result;
}

stdstring fullPathFromFilename( const stdstring& filename )
{
    TCHAR path[MAX_PATH];
    TCHAR *fname_ptr = NULL;
    GetFullPathName(RI.Filename, sizeof path / sizeof path[0], path, &fname_ptr);
    *fname_ptr = 0;
    return stdstring( path );
}

// Strip the filename out of a path. E.g. src/foo.s -> src/
stdstring stripFilename( const stdstring& filename )
{
    // Scan backwards for / and \.
    for ( auto it = filename.rbegin(); it != filename.rend(); ++it )
    {
        if ( *it == '/' || *it == '\\' )
        {
            // Strip from here.
            return stdstring( filename.begin(), it.base() );
        }
    }

    // Didn't find / or \, so the whole string must be the filename.
    return stdstring();
}

unsigned char randomByte()
{
    return std::uniform_int_distribution< unsigned int >( 0, 255 )( rng );
}

void initializeRandom( void* memory, size_t memorySize, unsigned char mask,
    int forbid )
{
    unsigned char* ptr = static_cast< unsigned char* >( memory );
    for ( size_t i = 0; i != memorySize; ++i )
    {
        unsigned char newValue = 0;
        // Get new values until we get one that is not "forbid".
        // \note Default value of forbid is -1, which means that no values are
        //       forbidden.
        do newValue = randomByte() & mask; while ( newValue == forbid );
        *ptr++ = newValue;
    }
}

namespace LuaFunctions
{

int print( lua_State* L )
{
    // TODO: If multiple parameters are passed, concatenate them, separate with something (like Lua's default print would do).

    const char* str = lua_tostring( L, -1 );

    // NOTE: %s is used so that formatting characters can be safely used in the output string.
    EI.DbgOut( _T("%s"), cStringToTString( str ).c_str() );

    // Return the number of pushed return values.
    return 0;
}

int readRAM( lua_State* L )
{
    int addr = lua_tointeger( L, -1 );
    lua_pushinteger( L, CPU::ReadRAM( 0, addr ) );
    // Return the number of pushed return values.
    return 1;
}

int writeRAM( lua_State* L )
{
    int addr = lua_tointeger( L, -2 );
    int value = lua_tointeger( L, -1 );
    CPU::WriteRAM( 0, addr, value );
    // Mark the RAM as written to avoid bogus warnings.
    memory_access_info[ addr & 0x7FF ] = MEM_WRITTEN;

    // Return the number of pushed return values.
    return 0;
}

int getSymbolValueByName( lua_State* L )
{
    const char* ident = lua_tostring( L, -1 );
    // Look up the symbol(s) with the given name.
    const cc65_symbolinfo* symbols = cc65_symbol_byname( dbginfo, ident );
    // Create a table out of all of the symbols.
    int table_index = 1;
    if ( symbols && symbols->count != 0 )
    {
        lua_createtable( L, symbols->count, 0 );
        for ( unsigned i = 0; i < symbols->count; ++i )
        {
            const cc65_symboldata& sd = symbols->data[i];
            // Only include labels and equates (no imports).
            if ( sd.symbol_type == CC65_SYM_EQUATE || sd.symbol_type == CC65_SYM_LABEL )
            {
                // Push the value.
                lua_pushinteger( L, sd.symbol_value );
                // This does the equivalent of t[n] = v. It also pops the value from the stack.
                // Use 1-based indexing.
                lua_rawseti ( L, -2, table_index );
                ++table_index;
            }
        }
        // Table is at the top of the stack now.
    }

    // If nothing was added, push nil.
    if ( table_index == 1 )
    {
        lua_pushnil( L );
    }

    // Return the number of pushed return values (nil or a single table).
    return 1;
}

int getPPUCycles( lua_State* L )
{
    // lua_Number is a double, it can handle integers up to about 2^52, which should be fine for us.
    lua_pushnumber( L, PPU::TotalClockticks );
    // Return the number of pushed return values.
    return 1;
}

int getCPUCycles( lua_State* L )
{
    // lua_Number is a double, it can handle integers up to about 2^52, which should be fine for us.
    lua_pushnumber( L, PPUtoCPU( PPU::TotalClockticks ) );
    // Return the number of pushed return values.
    return 1;
}

int stop( lua_State* L )
{
    if ( NES::Running )
    {
        // Stop the emulator.
        NES::DoStop = TRUE;
        // Wait for the emulator to be actually stopped before continuing.
        while ( NES::Running )
        {
            Sleep( 1 );
        }

        // Return true, i.e. emulator WAS running before.
        lua_pushboolean( L, 1 );
    }
    else
    {
        // Return false, i.e. emulator was already stopped.
        lua_pushboolean( L, 0 );
    }

    // Return the number of pushed return values.
    return 1;
}

int run( lua_State* L )
{
    if ( NES::Running )
    {
        // Return false, i.e. emulator was already running.
        lua_pushboolean( L, 0 );
    }
    else
    {
        // Run the emulator.
        NES::Start( FALSE );
        // Return true, i.e. emulator WAS stopped before.
        lua_pushboolean( L, 1 );
    }

    // Return the number of pushed return values.
    return 1;
}

// Get a register or a CPU flag. Input parameter should be one of: A, X, Y, SP, P, PC, C, Z, I, D, V, N
// Flags are returned as booleans, others are returned as numbers.
int getRegister( lua_State* L )
{
    std::string reg_name( strToUpper( lua_tostring( L, -1 ) ) );

    // Make sure P reflects the state of the flags.
    CPU::JoinFlags();

#define CHECK_REGISTER_PARAM( reg, var, is_flag )           \
    if ( reg_name == reg )                                  \
    {                                                       \
        if ( is_flag )                                      \
        {                                                   \
            lua_pushboolean( L, CPU::var ? 1 : 0 );         \
        }                                                   \
        else                                                \
        {                                                   \
            lua_pushinteger( L, CPU::var );                 \
        }                                                   \
    }

         CHECK_REGISTER_PARAM( "A", A, false )
    else CHECK_REGISTER_PARAM( "X", X, false )
    else CHECK_REGISTER_PARAM( "Y", Y, false )
    else CHECK_REGISTER_PARAM( "SP", SP, false )
    else CHECK_REGISTER_PARAM( "P", P, false )
    else CHECK_REGISTER_PARAM( "PC", PC, false )
    else CHECK_REGISTER_PARAM( "C", FC, true )
    else CHECK_REGISTER_PARAM( "Z", FZ, true )
    else CHECK_REGISTER_PARAM( "I", FI, true )
    else CHECK_REGISTER_PARAM( "D", FD, true )
    else CHECK_REGISTER_PARAM( "V", FV, true )
    else CHECK_REGISTER_PARAM( "N", FN, true )
    else
    {
        lua_pushnil( L );
    }

#undef CHECK_REGISTER_PARAM

    // Return the number of pushed return values.
    return 1;
}

// Set a register or a CPU flag. First input parameter should be one of: A, X, Y, SP, P, PC, C, Z, I, D, V, N
int setRegister( lua_State* L )
{
    std::string reg_name( strToUpper( lua_tostring( L, -2 ) ) );
    // TODO: Does lua_tointeger work properly with boolean inputs?
    int value = lua_tointeger( L, -1 );

    enum SplitJoinType
    {
        SJT_NONE,
        SJT_SPLIT,
        SJT_JOIN
    };

    // If "P", we need to split the flags. If C/Z/I/D/V/N, we need to join the flags.
#define CHECK_REGISTER_PARAM( reg, var, split_join )        \
    if ( reg_name == reg )                                  \
    {                                                       \
        switch ( split_join )                               \
        {                                                   \
        case SJT_SPLIT:                                     \
            CPU::var = value;                               \
            CPU::SplitFlags();                              \
            break;                                          \
        case SJT_JOIN:                                      \
            CPU::var = value != 0;                          \
            CPU::JoinFlags();                               \
            break;                                          \
        default:                                            \
            CPU::var = value;                               \
        }                                                   \
    }

#pragma warning( push )
#pragma warning( disable : 4800 )
         CHECK_REGISTER_PARAM( "A", A, SJT_NONE )
    else CHECK_REGISTER_PARAM( "X", X, SJT_NONE )
    else CHECK_REGISTER_PARAM( "Y", Y, SJT_NONE )
    else CHECK_REGISTER_PARAM( "SP", SP, SJT_NONE )
    else CHECK_REGISTER_PARAM( "P", P, SJT_SPLIT )
    else CHECK_REGISTER_PARAM( "PC", PC, SJT_NONE )
    else CHECK_REGISTER_PARAM( "C", FC, SJT_JOIN )
    else CHECK_REGISTER_PARAM( "Z", FZ, SJT_JOIN )
    else CHECK_REGISTER_PARAM( "I", FI, SJT_JOIN )
    else CHECK_REGISTER_PARAM( "D", FD, SJT_JOIN )
    else CHECK_REGISTER_PARAM( "V", FV, SJT_JOIN )
    else CHECK_REGISTER_PARAM( "N", FN, SJT_JOIN )
#pragma warning( pop )

#undef CHECK_REGISTER_PARAM

    // Return the number of pushed return values.
    return 0;
}

int getMainWindowHandle( lua_State* L )
{
    // Hope to Lord (Beelzebub) that HWND fits in 32 bits or something.
    lua_pushinteger( L, lua_Integer( hMainWnd ) );
    // Return the number of pushed return values.
    return 1;
}

int setAfterFrameHook( lua_State* L )
{
    // Set a hook to a function, or clear it (by passing nil).

    // Get a reference to the object (function) on top of the stack (and pop it).
    int ref = luaL_ref( L, LUA_REGISTRYINDEX );
    if ( ref == LUA_REFNIL )
    {
        ref = LUA_NOREF;
    }
    // Save the old value.
    int prev_callback = lua_after_frame_callback_ref;
    // Set the callback to a function (or to LUA_NOREF).
    lua_after_frame_callback_ref = ref;
    // Return the previous callback, unless there was none, in which case return nil.
    if ( prev_callback != LUA_NOREF )
    {
        // Push the old function on the stack.
        lua_rawgeti( L, LUA_REGISTRYINDEX, prev_callback );
        // Remove the reference.
        luaL_unref( L, LUA_REGISTRYINDEX, prev_callback );
    }
    else
    {
        lua_pushnil( L );
    }
    // Return the number of pushed return values.
    return 1;
}

int getMouseState( lua_State* L )
{
    lua_pushinteger( L, mouse_pos.x );
    lua_pushinteger( L, mouse_pos.y );
    lua_pushboolean( L, mouse_btn_left );
    lua_pushboolean( L, mouse_btn_right );
    lua_pushboolean( L, mouse_btn_middle );
    // Return the number of pushed return values.
    return 5;
}

int getControllerState( lua_State* L )
{
    int controller = lua_tointeger( L, -1 );
    if ( controller >= 1 && controller <= NUM_JOYPADS )
    {
        // If joypad is plugged in, return the 8 bits as an integer.
        // Otherwise return nil.
        int state = joy_bits[ controller - 1 ];
        if ( state >= 0 )
        {
            lua_pushinteger( L, state );
        }
        else
        {
            lua_pushnil( L );
        }
    }
    else
    {
        lua_pushnil( L );
    }
    // Return the number of pushed return values.
    return 1;
}

// Reads a value from 6502 memory space, be it RAM, PRG-ROM or whatever.
// Returns nil if the address couldn't be read (for registers, MMC5
// ExRAM(?) etc).
int readMemory( lua_State* L )
{
    int addr = lua_tointeger( L, -1 );
    // If value is valid, return it. Otherwise return nil. Nil will
    // be returned, for example, when trying to read the PPU registers.
    unsigned short value = Debugger::DebugMemCPUEx( addr );
    if ( value >= 0 && value <= 255 )
    {
        lua_pushinteger( L, value );
    }
    else
    {
        lua_pushnil( L );
    }
    // Return the number of pushed return values.
    return 1;
}

// Jumps to a subroutine defined in 6502 code.
int jsr( lua_State* L )
{
    int addr = lua_tointeger( L, -1 );

    luaCallbackSavedPc.push( CPU::PC );
    CPU::PC = kLuaJsrCallbackAddress;
    luaCallbackInjectAddress = addr;

    // Let the emulator thread run.
    SetEvent( lua_trigger_done_event );

    // Wait for an event from emulator thread signifying that the execution
    // has returned to the call site.
    HANDLE handles[] = {
        luaCallbackReturnEvent,
        lua_trigger_event
    };
    for ( ;; )
    {
        DWORD ret = WaitForMultipleObjects( 2, handles, FALSE, INFINITE );
        if ( ret == WAIT_OBJECT_0 + 0 ) //luaCallbackReturnEvent
        {
            break;
        }
        else if ( ret == WAIT_OBJECT_0 + 1 ) //lua_trigger_event
        {
            doLuaTrigger();
        }
    }

    return 0;
}

static const luaL_Reg funcs[] =
{
    { "print", print },
    { "readRAM", readRAM },
    { "writeRAM", writeRAM },
    { "getSymbolValueByName", getSymbolValueByName },
    { "getPPUCycles", getPPUCycles },
    { "getCPUCycles", getCPUCycles },
    { "stop", stop },
    { "run", run },
    { "getRegister", getRegister },
    { "setRegister", setRegister },
    //{ "getMainWindowHandle", getMainWindowHandle },
    { "setAfterFrameHook", setAfterFrameHook },
    { "getMouseState", getMouseState },
    { "getControllerState", getControllerState },
    { "readMemory", readMemory },
    { "jsr", jsr },
    { NULL, NULL }
};

} // namespace LuaFunctions

void registerLuaFunctions( lua_State* L )
{
    luaL_newlib( L, LuaFunctions::funcs );
    lua_setglobal( L, "NDX" );

    // Run the common Lua stuff (wrappers, helpers and so on).
    // Build the path.
    stdstring prog_path( ProgPath );
    prog_path += _T( "init.lua" );
    // Execute.
    int error = luaL_dofile( L, tStringToCString( prog_path ).c_str() );
    if ( error )
    {
        EI.DbgOut( _T( "Lua error: %s" ), cStringToTString( lua_tostring( L, -1 ) ).c_str() );
        lua_pop( L, 1 );
    }
}

// ----------------------------------------------------------------------------

#ifdef POWERPAK_EMULATION
namespace PowerPak
{
    const int PRGBANK           = 0x4200;
    const int CHRBANK           = 0x4201;
    const int FPGAREAD          = 0x4208;

    const int CARDDATAREAD      = 0x5000;

    const int CARDDATAWRITE     = 0x5400;
    const int CARDSECTORCOUNT   = 0x5402;
    const int CARDLBA0          = 0x5403;
    const int CARDLBA1          = 0x5404;
    const int CARDLBA2          = 0x5405;
    const int CARDLBA3          = 0x5406;
    const int CARDCOMMAND       = 0x5407;

    const int FPGAPROGRAM       = 0x5800;
    const int FPGADATA          = 0x5C00;
    const int CARDSTATUS        = 0x50F7;

    const int SECTOR_SIZE       = 512;

    const int PRG_BANK_SIZE     = 8192;

    // NOTE: The disk image has to have a partition table included in it.
    //       "Disk Management" tool in Windows does the job nicely (or
    //       the DISKPART tool)
    const std::string CF_IMAGE_PATH = "E:\\dev\\powermappers\\tools\\drive.vhd";

    // ------------------------------------------------------------------------

    int sectorCount;
    int lba;
    int command;
    std::fstream cfImage;
    bool couldntOpenCfImage;

    /// Current byte offset in cfImage
    unsigned int imageOffset;

    /// 512 KB of PRG-RAM
    std::array< unsigned char, 512*1024 > prgRam;

    /// Currently mapped in PRG bank.
    int prgBank;

    // \todo 32 KB of WRAM

    // ------------------------------------------------------------------------

    const int NUM_PRG_BANKS = prgRam.size() / PRG_BANK_SIZE;

    // ------------------------------------------------------------------------

    unsigned char& prgRamAt( int addr )
    {
        int offset = ( prgBank & ( NUM_PRG_BANKS - 1 ) ) *
            PRG_BANK_SIZE + ( ( addr - 0x6000 ) & ( PRG_BANK_SIZE - 1 ) );
        return prgRam[offset];
    }

    bool openCfImage()
    {
        if ( couldntOpenCfImage )
            return false;

        if ( cfImage.is_open() )
            return true;

        cfImage.open( CF_IMAGE_PATH, std::ios::binary | std::ios::in |
            std::ios::out );

        if ( !cfImage.is_open() )
        {
            EI.DbgOut( _T( "Couldn't open CF image" ) );
            couldntOpenCfImage = true;
            return false;
        }

        return true;
    }

    bool startReadWrite()
    {
        if ( !openCfImage() )
            return false;

        if ( ( imageOffset & ( SECTOR_SIZE - 1 ) ) == 0 )
            cfImage.seekg( imageOffset );

        return true;
    }

    int read( int addr )
    {
        // \todo Handle mirrors.

        switch ( addr )
        {
        // Battery save register (\todo handle writes)
        case FPGAREAD:
            return 0;

        // CF card status
        case CARDSTATUS:
        {
            // 0xFF = no card
            // return 0xFF;

            // Status register bits:
            // BUSY RDY DWF DSC DRQ CORR 0 ERR
            // If BUSY=1, other bits are not valid
            // If RDY=1, CF is ready to perform operations
            // If DWF=1, a write faul has occurred
            // If DSC=1, CF is ready
            // If DRQ=1, CF is ready for data transfer
            // If CORR=1, a correctable error has occurred
            // If ERR=1, error has occurred (more info from another register)

            // Ready, no errors.
            return 0x58;
        }

        case CARDDATAREAD:
        {
            if ( !startReadWrite() )
                return -1;

            // EI.DbgOut( _T( "Reading CF from '%d'" ), imageOffset );

            ++imageOffset;

            int data = cfImage.get();

            return data;
        }

        default:
            if ( addr >= 0x6000 && addr < 0x8000 )
            {
                if ( prgBank & 0x80 )
                    ; // \todo WRAM
                else
                    return prgRamAt( addr );
            }
            else if ( addr >= 0x4200 && addr < 0x6000 )
            {
                EI.DbgOut( _T( "Reading '%04x'" ), addr );
            }
        }

        return -1;
    }

    void write( int addr, unsigned char data )
    {
        // NOTE: 0x40 for PRGBANK enables CHR-RAM writing in the firmware
        //       FPGA config.

        // NOTE: MAPPERWR (@8000) should work automatically since UxROM is
        //       used as the iNES mapper number.

        switch ( addr )
        {
        case FPGAPROGRAM:
        case FPGADATA:
        case PRGBANK:
            break;

        case CARDSECTORCOUNT:
            // Sector count for a read/write operation.
            sectorCount = data;
            break;

        case CARDLBA0:
        case CARDLBA1:
        case CARDLBA2:
        case CARDLBA3:
        {
            int offset = addr - CARDLBA0;
            int shift = 8 * offset;
            int mask = 0xFF << shift;
            if ( addr == CARDLBA3 )
                data &= 0xF;
            // Mask out the old data, OR in the new data.
            // NOTE: Only 28 bits used (rest to select between LBA/CHS etc).
            lba = ( lba & ~mask ) | ( data << shift );

            if ( addr == CARDLBA3 )
                imageOffset = SECTOR_SIZE * lba;

            break;
        }

        case CARDCOMMAND:
        {
            // Commands:
            //     0x90: Drive Diagnostic
            //     0xEC: Identify Drive
            //     0x20: Read Sectors
            //     0xEF: Set Features
            //     0x30: Write Sectors
            command = data;
            break;
        }

        case CARDDATAWRITE:
        {
            if ( !startReadWrite() )
                return;

            // EI.DbgOut( _T( "Writing CF to '%d'" ), imageOffset );

            ++imageOffset;

            cfImage.put( data );
        }

        default:
            // Map 512 KB of PRG-RAM at 6000-7FFF, banked by writes to
            // PRGBANK. Used for directory listings and maybe some other stuff.
            if ( addr >= 0x6000 && addr < 0x8000 )
            {
                if ( prgBank & 0x80 )
                    ; // \todo WRAM
                else
                    prgRamAt( addr ) = data;
            }
            else
            {
                // if ( addr >= 0x4200 && addr < 0x6000 )
                //     EI.DbgOut( _T( "Writing '%04x' <= '%02x'" ), addr, data );
            }
            break;
        }
    }
}
#endif

/// Called when memory is read by the CPU, and the read isn't handled by any
/// other handler (such as a memory device, mapper, etc).
/// Should return -1 for open bus.
int readMemory( int addr )
{
#ifdef POWERPAK_EMULATION
    int result = PowerPak::read( addr );
    if ( result != -1 )
        return result;
#endif

    if ( !luaCallbackSavedPc.empty() )
    {
        // Inject a JSR
        if ( addr >= kLuaJsrCallbackAddress && addr < kLuaJsrCallbackReturn )
        {
            const int kJsrOpcode = 0x20;
            int relative = addr - kLuaJsrCallbackAddress;
            switch ( relative )
            {
            case 0: return kJsrOpcode;
            case 1: return luaCallbackInjectAddress & 0xFF;
            case 2: return luaCallbackInjectAddress >> 8;
            default: assert( false );
            }
        }
    }

    return -1;
}

void writeMemory( int addr, unsigned char data )
{
#ifdef POWERPAK_EMULATION
    PowerPak::write( addr, data );
#endif
}

void writeTo2007WhenRendering()
{
    if ( ppu2007WriteDiagnosticShown )
        return;

    EI.DbgOut( _T( "Warning: Writing to $2007 while PPU is " )
        _T( "rendering (PC = $%X)  (further messages suppressed)" ),
        CPU::OpAddr );

    ppu2007WriteDiagnosticShown = true;
}

void invalidBlackUsedInRendering()
{
    if ( invalidBlackDiagnosticShown )
        return;

    // \todo May not be safe to call EI.DbgOut from the rendering thread...
    EI.DbgOut( _T( "Warning: Invalid black ($0D) used in " )
        _T( "rendering  (further messages suppressed)" ) );

    invalidBlackDiagnosticShown = true;
}

} // namespace DebugExt

// Provide a dummy main() for IUP to call. For whatever reason, their library
// has a WinMain which calls main(). However, since this project is compiled
// as Unicode, that WinMain never gets called.
int main( int, char** )
{
}
