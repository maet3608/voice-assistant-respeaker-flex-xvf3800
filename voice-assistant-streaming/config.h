// Every pin, size and string of the streamed voice assistant.

#pragma once

#include <Arduino.h>

// API
constexpr const char *apiUrl = "https://api.openai.com/v1/chat/completions";
constexpr const char *audioModel = "gpt-audio-1.5";
constexpr const char *audioVoice = "alloy";
constexpr const char *systemPrompt =
  "You are a friendly voice assistant. Answer in at most 40 words. Your answer "
  "is spoken aloud, so use plain sentences without lists, markdown or emoji. "
  "You can blink the built-in LED with the blink_led function.";
constexpr const char *userPrompt = "Answer the question in this recording.";

constexpr int blinkIntervalMs = 500;
constexpr int maxBlinkCount = 10;

constexpr const char *streamFormat = "pcm16";
constexpr uint32_t streamRate = 24000;

constexpr size_t ringBytes = 32768;

constexpr uint32_t playTaskStackBytes = 4096;
constexpr int playTaskPriority = 3;

// The bus between XIAO and XVF3800
constexpr int busRate = 48000;
constexpr int busChannels = 2;
constexpr int busBits = 32;
constexpr int micChannel = 1; // the microphone audio is in slot 1

static_assert((uint32_t)busRate == streamRate * 2,
              "the playback path doubles 24 kHz to 48 kHz");

constexpr uint32_t recordRate = 16000;
constexpr int decimation = busRate / recordRate;

constexpr size_t chunkBytes = 4096;
static_assert(chunkBytes % (busChannels * sizeof(int32_t)) == 0,
              "a chunk must hold whole frames");

// Pins and the AIC3104 codec
constexpr int pinBck = 8;           // D9 I2S_BCLK
constexpr int pinWs = 7;            // D8 I2S_LRCK
constexpr int pinDataOut = 44;      // D7 I2S_DATA0 to the XVF3800
constexpr int pinDataIn = 43;       // D6 I2S_DATA1 from the XVF3800
constexpr int pinLed = LED_BUILTIN; // GPIO21, active low
constexpr int pinButton = 1;        // D0/GPIO1 to GND, active low, pulled up

constexpr int i2cSda = 5; // D4
constexpr int i2cScl = 6; // D5

constexpr uint8_t aic3104Address = 0x18;
constexpr uint8_t aic3104PageCtrl = 0x00;
constexpr uint8_t aic3104LeftDac = 0x2B;
constexpr uint8_t aic3104RightDac = 0x2C;
constexpr uint8_t aic3104HpLeft = 0x33;
constexpr uint8_t aic3104HpRight = 0x41;
constexpr uint8_t aic3104LineLeft = 0x56;
constexpr uint8_t aic3104LineRight = 0x5D;

constexpr float codecVolumeDb = -12.0f;

// Recording behaviour
constexpr uint32_t maxRecordMs = 10000;
constexpr uint32_t listenTimeoutMs = 6000;
constexpr uint32_t trailingSilenceMs = 900;
constexpr int16_t speechLevel = 600; // about -35 dBFS

// Buffer sizes in PSRAM
constexpr size_t audioCapacity = (size_t)recordRate * (maxRecordMs / 1000) * 2;
constexpr size_t maxWavBytes = 44 + audioCapacity;
constexpr size_t maxBase64Bytes = (maxWavBytes / 3 + 1) * 4 + 4;
constexpr size_t requestCapacity = maxBase64Bytes + 4096;

constexpr size_t eventBytes = 32768;
constexpr size_t decodedBytes = eventBytes / 4 * 3;
