// Sends the request and plays the answer while it streams in.

#pragma once

#include <Arduino.h>

bool streamAnswer(size_t length, bool expectAudio = true);
