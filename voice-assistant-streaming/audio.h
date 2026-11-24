// The recording buffer in PSRAM, the I2S bus and the streamed playback.

#pragma once

#include <Arduino.h>

bool audioBegin();

size_t recordSpeech();

const uint8_t *recordedWav();

int32_t lastRecordPeak();

void requestStarted();

void queueSamples(const uint8_t *data, size_t length);

bool playbackFailed();

void endPlayback();

float playedSeconds();
