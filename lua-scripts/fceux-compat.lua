-- FCEUX compatibility stuff for NintendulatorDX.
-- by thefox//aspekt 2012

local hook_canvas = cd.CreateCanvas( cd.IMAGERGB, "283x240 -a" )
hook_canvas:SetBackground( cd.EncodeAlpha( cd.BLACK, 0 ) )
hook_canvas:Font( "Times New Roman", cd.PLAIN, 6 )
hook_canvas:TextAlignment( cd.NORTH_WEST )

local function fixY( y )
    return -y + 239
end

local function strToColor( str )
    if str:sub( 1, 1 ) == "#" then
        local r = tonumber( "0x" .. str:sub( 2, 3 ) )
        local g = tonumber( "0x" .. str:sub( 4, 5 ) )
        local b = tonumber( "0x" .. str:sub( 6, 7 ) )
        return cd.EncodeColor( r, g, b )
    else
        -- Try to retrieve the color constant from the "cd" table
        -- (it doesn't contain every color that FCEUX defines, though.)
        return cd[ str:upper() ]
    end
end

local LEFT_BORDER = 1+15

gui = {
    drawbox = function( x1, y1, x2, y2, color )
        local x1 = x1 + LEFT_BORDER
        local x2 = x2 + LEFT_BORDER
        hook_canvas:SetForeground( strToColor( color ) )
        hook_canvas:Rect( x1, x2, fixY( y1 ), fixY( y2 ) )
    end,
    text = function( x, y, str )
        local x = x + LEFT_BORDER
        local y = fixY( y )
        hook_canvas:SetForeground( cd.EncodeAlpha( cd.BLACK, 200 ) )
        hook_canvas:Box( hook_canvas:GetTextBox( x, y, str ) )
        hook_canvas:SetForeground( cd.WHITE )
        hook_canvas:Text( x, y, str )
    end,
    drawpixel = function( x, y, color )
        local x = x + LEFT_BORDER
        hook_canvas:Pixel( x, fixY( y ), strToColor( color ) )
    end,
    drawline = function( x1, y1, x2, y2, color )
        local x1 = x1 + LEFT_BORDER
        local x2 = x2 + LEFT_BORDER
        hook_canvas:SetForeground( strToColor( color ) )
        hook_canvas:Line( x1, fixY( y1 ), x2, fixY( y2 ) )
    end,
}

memory = {
    readbyte = NDX.readRAM,
    writebyte = NDX.writeRAM,
}

input = {
    get = function()
        local x, y, left, right, middle = NDX.getMouseState()
        -- TODO: Keyboard state.
        return {
            xmouse = x-LEFT_BORDER,
            ymouse = y,
            leftclick = left or nil,
            rightclick = right or nil,
            middleclick = middle or nil
        }
    end,
}

joypad = {
    read = function( player )
        local state_bits = NDX.getControllerState( player )
        if state_bits then
            return {
                A       = bit32.btest( state_bits, 0x01 ) or nil,
                B       = bit32.btest( state_bits, 0x02 ) or nil,
                select  = bit32.btest( state_bits, 0x04 ) or nil,
                start   = bit32.btest( state_bits, 0x08 ) or nil,
                up      = bit32.btest( state_bits, 0x10 ) or nil,
                down    = bit32.btest( state_bits, 0x20 ) or nil,
                left    = bit32.btest( state_bits, 0x40 ) or nil,
                right   = bit32.btest( state_bits, 0x80 ) or nil,
            }
        else
            return { }
        end
    end,
}

AND = bit32.band
OR = bit32.bor
XOR = bit32.bxor
-- TODO: BIT

FCEU = { }

fceuxCompat = { }

function fceuxCompat.startDraw()
    hook_canvas:Activate()
    hook_canvas:Clear()
end

function fceuxCompat.endDraw()
    return hook_canvas
end
