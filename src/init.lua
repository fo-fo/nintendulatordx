NDX.print( "Initializing Lua..." )

------------------------------------------------------------------------

-- Create the RAM table (just a proxy). Returns a byte from RAM. This
-- can be indexed using an integer or a symbol identifier, e.g.
--   foo = RAM[ 0x123 ]
--   bar = RAM.some_symbol
RAM = { }
-- Create the metatable for it.
local RAM_metatable = {
    __index = function ( t, k )
        if type( k ) == "string" then
            local syms = NDX.getSymbolValueByName( k )
            if #syms > 1 then
                error( "More than one symbol returned when indexing " ..
                    "RAM with a symbol (read)", 2 )
            end
            return NDX.readRAM( syms[ 1 ] )
        else
            return NDX.readRAM( k )
        end
    end,
    __newindex = function ( t, k, v )
        if type( k ) == "string" then
            local syms = NDX.getSymbolValueByName( k )
            if #syms > 1 then
                error( "More than one symbol returned when indexing " ..
                    "RAM with a symbol (write)", 2 )
            end
            NDX.writeRAM( syms[ 1 ], v )
        else
            NDX.writeRAM( k, v )
        end
    end
}
setmetatable( RAM, RAM_metatable )

------------------------------------------------------------------------

-- Create the SYM table. Returns value/address of an symbol. Returns a
-- a table (may return multiple values). Read only.
SYM = { }
-- Create the metatable for it.
local SYM_metatable = {
    -- NOTE: Returns a table.
    __index = function ( t, k ) return NDX.getSymbolValueByName( k ) end,
	__newindex = function ( ) error( "Modifying SYM table is not allowed", 2 ) end
}
setmetatable( SYM, SYM_metatable )

------------------------------------------------------------------------

-- Create the REG table. Returns/sets value of a register/flag.
-- Parameter has to be one of: A, X, Y, SP, P, PC, C, Z, I, D, V, N
REG = { }
local REG_metatable = {
    __index = function ( t, k ) return NDX.getRegister( k ) end,
    __newindex = function ( t, k, v ) NDX.setRegister( k, v ) end
}
setmetatable( REG, REG_metatable )

------------------------------------------------------------------------

-- Set up a simple Lua Console using IUP.
local ml_console = iup.multiline {
    expand      = "YES",
    font        = "Consolas, 10",
    readonly    = "YES"
}
local btn_load_and_run = iup.button {
    title       = "Load and Run"
}
local btn_browse = iup.button {
    title       = "Browse...",
    padding     = "3x3"
}
local text_filename = iup.text {
    expand      = "HORIZONTAL",
    padding     = "3x3"
}
local btn_run = iup.button {
    title       = "Run",
    padding     = "15x3"
}
local btn_pause = iup.button {
    title       = "Pause",
    padding     = "15x3"
}
local file_dlg = iup.filedlg {
    dialogtype  = "OPEN",
    title       = "Load Lua Script",
    extfilter   = "Lua Script Files (*.lua)|*.lua|All Files (*.*)|*.*",
}
function btn_browse:action()
    file_dlg:popup( iup.ANYWHERE, iup.ANYWHERE )
    local status = file_dlg.status
    if status == "0" then
        -- Result filename in file_dlg.value
        text_filename.value = file_dlg.value
    elseif status == "-1" then 
        -- User canceled, nothing to do.
    else
        -- Should never happen.
        assert( false )
    end
end
function btn_run:action()
    -- If paused, resume first.
    if btn_pause.title == "Resume" then
        btn_pause:action()
    end
    local filename = text_filename.value
    if filename == "" then
        print( "(Lua Console) Select a file first!" )
        return
    end
    print( string.format( "(Lua Console) Compiling and executing" ..
        " '%s'...", filename ) )
    local file, comp_err = loadfile( filename )
    if file then
        local success, msg = pcall( file )
        if success then
            print( "(Lua Console) Execution finished succesfully." )
        else
            print( "(Lua Console) An error occured while executing:" )
            print( msg )
        end
    else
        print( "(Lua Console) An error occured while compiling:" )
        print( comp_err )
    end
end
local prev_after_frame_hook = nil
function btn_pause:action()
    if btn_pause.title == "Pause" then
        -- Disable all hooks.
        prev_after_frame_hook = NDX.setAfterFrameHook( nil )
        btn_pause.title = "Resume"
    else
        -- Re-enable all hooks.
        NDX.setAfterFrameHook( prev_after_frame_hook )
        btn_pause.title = "Pause"
    end
end
local box_console_controls = iup.hbox {
    btn_browse,
    text_filename,
    btn_run,
    btn_pause;
    expand      = "YES",
    margin      = "4x4",
    gap         = 10,
    alignment   = "ACENTER"
}
local box_console = iup.vbox {
    ml_console,
    box_console_controls;
    expand      = "YES",
    margin      = "2x2"
}
local dlg_console = iup.dialog {
    box_console;
    title           = "Lua Console",
    minsize         = "450x350"
}

------------------------------------------------------------------------

-- Replace print function with one that prints to the Lua Console.
function print( ... )
    local arg = { ... }
    local result_str = ""
    for i, v in ipairs( arg ) do
        result_str = result_str .. tostring( v ) .. "\t"
    end
    local count = ml_console.count
    -- This works better than scrolltopos.
    ml_console.selectionpos = string.format( "%d:%d", count, count )
    ml_console.insert = result_str .. "\n"
end

------------------------------------------------------------------------

-- Replace require with a function that also looks for Lua sources in
-- the source directory of the calling function.
local lua_require = require
function require( what )
    -- Get the source file name of the calling function.
    local source = debug.getinfo( 2, "S" ).source
    -- Is it a file name?
    if source:sub( 1, 1 ) == "@" then
        -- OK, search this directory.
        -- Find the last occurrence of path separator and trim the filename.
        -- The result contains the last path separator.
        local dir = source:sub( 2, 1 + #source - source:reverse():find( "[/\\]" ) )
        -- Check if the source directory contains "what".
        local full_path = dir .. what .. ".lua"
        local file = io.open( full_path )
        if file then
            -- OK, found it, load it with the full path.
            file:close()
            return dofile( full_path )
        end
    end
    -- Let the default require try if the file wasn't found in the source
    -- directory or a full source filename wasn't specified for the calling
    -- function.
    return lua_require( what )
end

------------------------------------------------------------------------
 
-- Display the console.
dlg_console:show()

NDX.print( "Lua initialization complete." )
