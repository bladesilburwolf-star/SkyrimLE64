#pragma once
#include <stdio.h>
// Hooks the real dinput8.dll keyboard device (shared vtable), so it also works under SKSE's wrapper.
// While blocking, key PRESSES for Mario's keys (W A S D, Space, Left Ctrl, F) are hidden from the game.
bool InputBlockInstall(FILE* log);
void InputBlockSet(bool block);
