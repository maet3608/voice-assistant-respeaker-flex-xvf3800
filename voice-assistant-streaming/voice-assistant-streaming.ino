// www.makerguides.com
// board: Seeed XIAO ESP32S3 (esp32:esp32:XIAO_ESP32S3)
// esp32 core: 4.0.0
// libraries:
// - audio-tools 1.2.6
// - ArduinoJson 7.4.3
// tool settings:
// - USB CDC On Boot : Enabled
// - PSRAM : OPI PSRAM
// prerequisites:
// - XVF3800 with an I2S firmware, not the USB firmware
// - 2.4 GHz WiFi and an OpenAI API key in credentials.h
// - push button between D0 (GPIO1) and GND
// - speaker on the JST connector (12 V supply) or headphones on the 3.5 mm jack

#include "AudioTools.h"
#include "assistant.h"
#include "audio.h"
#include "config.h"
#include "hardware.h"
#include "parser.h"
#include "request.h"
#include "tools.h"

void doTurn() {
  Serial.println("\nListening, ask your question");

  parserReset(); // a call from the last turn must not run again

  setLed(true); // the LED doubles as the recording indicator
  size_t wavBytes = recordSpeech();
  setLed(false);
  if (wavBytes == 0) {
    Serial.printf("Nothing to send, loudest peak %ld against threshold %d\n",
                  (long)lastRecordPeak(), speechLevel);
    Serial.println("If you did speak, lower speechLevel and upload again");
    return;
  }

  // text only: audio asked for together with a function is dropped
  size_t requestBytes = buildDecisionRequest(wavBytes);
  if (requestBytes == 0) {
    return;
  }
  streamAnswer(requestBytes, false);

  ToolCall call;
  if (parserToolCall(call)) {
    String result = runTool(call);
    requestBytes = buildAnswerRequest(wavBytes, &call, result.c_str());
  } else {
    requestBytes = buildAnswerRequest(wavBytes);
  }
  if (requestBytes > 0) {
    streamAnswer(requestBytes);
  }

  Serial.println("Done, press the D0 button for the next question");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  AudioLogger::instance().begin(Serial, AudioLogger::Info);

  pinMode(pinLed, OUTPUT);
  setLed(false);
  pinMode(pinButton, INPUT_PULLUP);

  Serial.printf("\nPSRAM: %u bytes, free heap: %u bytes\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
  if (!audioBegin() || !requestBegin() || !parserBegin()) {
    Serial.println("PSRAM allocation failed");
    Serial.println("Set PSRAM to OPI PSRAM in the Tools menu and upload again");
    return;
  }
  Serial.printf("buffers: %u + %u + %u bytes, %u bytes PSRAM free\n",
                (unsigned)maxWavBytes, (unsigned)requestCapacity,
                (unsigned)ringBytes, (unsigned)ESP.getFreePsram());

  setupCodec();
  if (!connectWiFi()) {
    return;
  }

  delay(1000); // let the XVF3800 settle after power up
  Serial.println("Ready, press the D0 button and ask a question");
}

void loop() {
  if (digitalRead(pinButton) == LOW) {
    doTurn();
    delay(500); // one turn per press
  }
}
