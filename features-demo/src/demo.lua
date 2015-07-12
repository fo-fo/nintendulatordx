-- This file is executed from within the luaDemo function
-- of main.s.

print "In demo.lua!"

-- Reset can be detected like this. demo_running is set to true at the end of
-- this script.
if demo_running then
    print "demo.lua already active, exiting!"
    return
end

print "First 8 bytes from RAM:"
for i = 0, 7 do
    print( string.format( "  %02X", RAM[ i ] ) )
end

print( string.format( "Value of symbol 'reset' is $%04X", SYM.reset[ 1 ] ) )

function luaCallbackTest( number )
    print( string.format( "In luaCallbackTest, got %d", number ) )
    print( "Calling callbackTest2() in 6502" )
    NDX.jsr( SYM.callbackTest2[ 1 ] )
end

-- Lua code can call back into 6502 code. 6502 code can then call back to
-- Lua as well.
print( "Calling back to 6502 code from Lua..." )
NDX.jsr( SYM.callbackTest[ 1 ] )
print( string.format( "Register A after callback: %d", REG.A ) )

-- Create a dialog with a single button with IUP.
local press_me_button = iup.button
{
    title="Press me!",
    expand="YES"
}
local dialog = iup.dialog
{
    press_me_button;
    title="NintendulatorDX Feature Demo",
    size="200x200"
}

-- This function is called when the button is pressed.
function press_me_button:action()
    -- Stop the emulator thread, so we can safely access its state.
    local was_running = NDX.stop()

    -- Show a message for shits and giggles, in an actual application
    -- we could of course continue knowing that the emulator is not
    -- running.
    if not was_running then
        iup.Message( "Emulator not running", "Looks like the emulator " ..
            "is not running, run it (F2) before pressing the button." )
        return
    end

    -- Set a variable in RAM to 1 so the 6502 code may continue.
    RAM.lua_button_pressed = 1

    -- Resume the emulator thread.
    NDX.run()
end

-- Render something on the screen while waiting for the button
-- to be pressed.
local hook_canvas = cd.CreateCanvas( cd.IMAGERGB, "283x240 -a" )
hook_canvas:SetBackground( cd.EncodeAlpha( cd.BLACK, 0 ) )
hook_canvas:Font( "Times New Roman", cd.PLAIN, 12 )
hook_canvas:TextAlignment( cd.CENTER );

-- Set a hook callback to run after each frame has finished rendering.
-- NOTE: The emulator thread is automatically blocked from running
-- when this function is called.
NDX.setAfterFrameHook( function()
    -- Clear the canvas.
    hook_canvas:Activate()
    hook_canvas:Clear()

    -- Set the foreground color alpha based on time.
    local text_alpha = math.sin( os.clock() * 10 ) * 32 + 200
    hook_canvas:SetForeground( cd.EncodeAlpha( cd.WHITE, text_alpha ) )

    local text_orientation = math.sin( os.clock() * 1 ) * 10
    hook_canvas:TextOrientation( text_orientation )

    -- Display the mouse state.
    -- NOTE: The origo is at the bottom left corner of the screen.
    hook_canvas:Text(141, 120, string.format( "Mouse state: %d %d %s %s %s", NDX.getMouseState() ) )

    -- Display the controller state of both controllers.
    local ctl1_state = NDX.getControllerState( 1 ) or "n/a"
    local ctl2_state = NDX.getControllerState( 2 ) or "n/a"
    hook_canvas:Text(141, 120+32, string.format( "Controller state: %s - %s", ctl1_state, ctl2_state ) )

    -- Return the canvas.
    return hook_canvas
end )

dialog:show()

demo_running = true
