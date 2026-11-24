// Parses the server sent events of the answer, plays the audio and collects the
// function call.

#pragma once

#include <Arduino.h>

struct ToolCall {
  char id[48];
  char name[32];
  char arguments[256];
};

class SseSink : public Stream {
public:
  SseSink();

  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t length) override;

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  const char *bodyHead() const { return head; }

private:
  char head[1200] = {};
  size_t headLength = 0;
};

bool parserBegin();

bool parserSawAudio();
bool parserSawTranscript();
bool parserSawText();
bool parserSawToolCall();
bool parserToolCall(ToolCall &call);

const char *parserText();

void parserReset();
