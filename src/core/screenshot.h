#ifndef SCREENSHOT_H
#define SCREENSHOT_H

// Frame buffer dump for debugging the layout on real hardware. With
// SCREENSHOT_ENABLE set, the current frame goes to /sdcard/screens/<name>.bmp;
// SCREENSHOT_SERIAL additionally prints it as hex to the log, so the picture
// can be rebuilt on the host without pulling the card.
//
// The caller names the file (short 8.3 names, "clock", "wx-fc", ...). Both
// switches live in config.h, see features.h for the fallbacks. Call it from
// the UI task after a frame has been composed.
void screenshot_capture(const char *name);

#endif // SCREENSHOT_H
