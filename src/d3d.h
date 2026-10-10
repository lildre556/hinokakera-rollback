#pragma once
#include <stdbool.h>

// Saves the back buffer (call after Render, before Present) as a 24-bit BMP.
bool d3d_screenshot(void *device, const char *path);
