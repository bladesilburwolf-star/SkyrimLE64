#pragma once
#include <stdio.h>
// Reads the first gamepad directly and (while blocking) hides Mario's controls from the game by
// hooking XInputGetState: left stick, A, B, X and the left trigger are zeroed for Skyrim.
struct MarioPad { bool connected; float sx, sy; bool a, b, z; };   // sy: -1 = forward (joystick convention)
bool XInputInstall(FILE* log);
void XInputBlockSet(bool block);
MarioPad XInputReadPad();
