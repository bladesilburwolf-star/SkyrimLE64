#pragma once
#include <stdint.h>
#include <stdio.h>
// "Possession": hide the player's body and keep the player actor on Mario's position so Skyrim's
// own third-person camera follows Mario. Runs as a SKSE task on the game's main thread.
void PossessInit(FILE* log);
void PossessSet(bool on);                          // on: hide body + follow; off: show body again
void PossessFollow(const float posSky[3]);        // call once per frame with Mario's Skyrim position
