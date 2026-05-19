// firmware/main/screenshot.h
//
// USB-serial screenshot capture. Once started, the task waits for the text
// command "screenshot\n" on stdin (the USB console port) and responds with an
// SCAP-framed raw RGB565-LE image on stdout. Use scripts/screenshot.py on the
// host to capture and convert to PNG.
#pragma once

// Start the screenshot listener task. Call once after ui_start().
void screenshot_start(void);
