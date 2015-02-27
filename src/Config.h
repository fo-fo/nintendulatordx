#ifndef CONFIG_H
#define CONFIG_H

namespace Config
{
    // First non-blank pixel is the background color in grayscale (wiki calls
    // it "pulse").
    const int LEFT_BORDER_SIZE = 1+15;
    const int RIGHT_BORDER_SIZE = 11;
    const int SCREEN_SIZE_X = 256 + LEFT_BORDER_SIZE + RIGHT_BORDER_SIZE;
    const int SCREEN_SIZE_Y = 240;
    const int NTSC_FILTER_SCREEN_SIZE_X = 282;
    const int NTSC_FILTER_SCREEN_SIZE_Y = 240;
    const int NTSC_FILTER_WINDOW_X_MULT = 1;
    const int NTSC_FILTER_WINDOW_Y_MULT = 1;
}

#endif // !CONFIG_H
