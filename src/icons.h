#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>

// 10x10 colour weather icons, palette-indexed (0 = transparent). Mapped from
// Open-Meteo WMO weather codes by wx_from_code().

#define ICON_W 10
#define ICON_H 10

static const uint8_t ICON_PALETTE[7][3] = {
    {  0,   0,   0},   // 0 transparent
    {255, 150,   0},   // 1 sun       (amber)
    {150, 150, 165},   // 2 cloud     (grey)
    { 70, 130, 255},   // 3 rain      (blue)
    {220, 220, 235},   // 4 snow      (white)
    {255, 225,   0},   // 5 bolt      (yellow)
    { 90,  90, 110},   // 6 darkcloud (storm)
};

typedef enum { WX_SUN, WX_PARTLY, WX_CLOUD, WX_RAIN, WX_SNOW, WX_STORM, WX_COUNT } wx_icon_t;

static const uint8_t WX_ICONS[WX_COUNT][ICON_H][ICON_W] = {
    // SUN: amber disc + rays
    {{0,0,0,0,1,1,0,0,0,0},
     {0,0,0,0,0,0,0,0,0,0},
     {0,0,0,1,1,1,1,0,0,0},
     {0,0,1,1,1,1,1,1,0,0},
     {1,0,1,1,1,1,1,1,0,1},
     {1,0,1,1,1,1,1,1,0,1},
     {0,0,1,1,1,1,1,1,0,0},
     {0,0,0,1,1,1,1,0,0,0},
     {0,0,0,0,0,0,0,0,0,0},
     {0,0,0,0,1,1,0,0,0,0}},
    // PARTLY: small sun top-left + cloud lower-right
    {{0,1,1,0,0,0,0,0,0,0},
     {1,1,1,1,0,0,0,0,0,0},
     {0,1,1,1,0,0,0,0,0,0},
     {1,1,1,1,0,0,2,2,0,0},
     {0,1,1,0,0,2,2,2,2,2},
     {0,0,0,0,2,2,2,2,2,2},
     {0,0,0,2,2,2,2,2,2,2},
     {0,0,0,2,2,2,2,2,2,2},
     {0,0,0,0,2,2,2,2,2,0},
     {0,0,0,0,0,0,0,0,0,0}},
    // CLOUD
    {{0,0,0,0,0,0,0,0,0,0},
     {0,0,0,0,0,0,0,0,0,0},
     {0,0,0,2,2,2,0,0,0,0},
     {0,0,2,2,2,2,2,2,0,0},
     {0,2,2,2,2,2,2,2,2,0},
     {2,2,2,2,2,2,2,2,2,2},
     {2,2,2,2,2,2,2,2,2,2},
     {0,2,2,2,2,2,2,2,2,0},
     {0,0,0,0,0,0,0,0,0,0},
     {0,0,0,0,0,0,0,0,0,0}},
    // RAIN: cloud + slanted blue drops
    {{0,0,0,0,0,0,0,0,0,0},
     {0,0,0,2,2,2,0,0,0,0},
     {0,0,2,2,2,2,2,2,0,0},
     {0,2,2,2,2,2,2,2,2,0},
     {2,2,2,2,2,2,2,2,2,2},
     {0,2,2,2,2,2,2,2,2,0},
     {0,3,0,0,3,0,0,3,0,0},
     {0,0,3,0,0,3,0,0,3,0},
     {3,0,0,3,0,0,3,0,0,0},
     {0,0,0,0,0,0,0,0,0,0}},
    // SNOW: cloud + white flakes
    {{0,0,0,0,0,0,0,0,0,0},
     {0,0,0,2,2,2,0,0,0,0},
     {0,0,2,2,2,2,2,2,0,0},
     {0,2,2,2,2,2,2,2,2,0},
     {2,2,2,2,2,2,2,2,2,2},
     {0,2,2,2,2,2,2,2,2,0},
     {0,4,0,0,4,0,0,4,0,0},
     {0,0,0,4,0,0,4,0,0,0},
     {0,4,0,0,4,0,0,4,0,0},
     {0,0,0,0,0,0,0,0,0,0}},
    // STORM: dark cloud + yellow bolt
    {{0,0,0,0,0,0,0,0,0,0},
     {0,0,0,6,6,6,0,0,0,0},
     {0,0,6,6,6,6,6,6,0,0},
     {0,6,6,6,6,6,6,6,6,0},
     {6,6,6,6,6,6,6,6,6,6},
     {0,6,6,6,6,6,6,6,6,0},
     {0,0,0,0,5,5,0,0,0,0},
     {0,0,0,5,5,0,0,0,0,0},
     {0,0,5,5,5,5,0,0,0,0},
     {0,0,0,0,5,0,0,0,0,0}},
};

// Map a WMO weather code (Open-Meteo) to an icon.
static inline wx_icon_t wx_from_code(int code) {
    if (code <= 1)                 return WX_SUN;     // 0 clear, 1 mainly clear
    if (code == 2)                 return WX_PARTLY;  // partly cloudy
    if (code == 3)                 return WX_CLOUD;   // overcast
    if (code >= 45 && code <= 48)  return WX_CLOUD;   // fog
    if (code >= 51 && code <= 67)  return WX_RAIN;    // drizzle / rain
    if (code >= 71 && code <= 77)  return WX_SNOW;    // snow
    if (code >= 80 && code <= 82)  return WX_RAIN;    // rain showers
    if (code >= 85 && code <= 86)  return WX_SNOW;    // snow showers
    if (code >= 95)                return WX_STORM;   // thunderstorm
    return WX_CLOUD;
}

#endif // ICONS_H
