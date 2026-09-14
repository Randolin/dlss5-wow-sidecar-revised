#pragma once
#include <windows.h>

namespace sidecar {

// Whether the monitor a window sits on is in HDR mode.
//
// This is the question that decides the capture format: DWM composes an HDR
// desktop in scRGB, and Windows Graphics Capture will hand that surface over
// as FP16 if asked -- or tone-clipped into BGRA8 if not, which is what turns an
// HDR game's highlights into flat white. The game's own HDR setting is not
// consulted; what the compositor is doing is what matters.
bool DisplayIsHdr(HWND window);

// The SDR reference white the desktop is using, in nits, from the same
// output. On an HDR desktop Windows composes SDR content at this level (the
// "SDR content brightness" slider); it is what an HDR capture of an SDR window
// is scaled to. 0 when it cannot be read.
float SdrWhiteLevelNits(HWND window);

// The display's peak luminance in nits, as it reports it. Used to size the
// tone-map's headroom: the model's view should span what the display can
// actually show. 0 when it cannot be read.
float MaxLuminanceNits(HWND window);

}  // namespace sidecar
