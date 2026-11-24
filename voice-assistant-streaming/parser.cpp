#include "parser.h"

#include "audio.h"
#include "config.h"
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

namespace {

char *line = nullptr;
size_t lineLength = 0;
bool lineDropped = false;
bool overflowReported = false;

uint8_t *pcm = nullptr;

JsonDocument doc;

ToolCall toolCall = {};
char text[512] = {};
bool audioSeen = false;
bool transcriptSeen = false;
bool textSeen = false;

void handleAudio(const char *base64) {
  size_t length = strlen(base64) & ~(size_t)3;
  size_t decoded = 0;
  if (mbedtls_base64_decode(pcm, decodedBytes, &decoded,
                            (const unsigned char *)base64, length) != 0) {
    return;
  }
  audioSeen = true;
  queueSamples(pcm, decoded);
}

void handleText(const char *piece) {
  textSeen = true;
  strlcat(text, piece, sizeof(text));
}

void handleToolCalls(JsonArray calls) {
  for (JsonObject call : calls) {
    if (const char *id = call["id"]) {
      strlcpy(toolCall.id, id, sizeof(toolCall.id));
    }
    if (const char *name = call["function"]["name"]) {
      strlcpy(toolCall.name, name, sizeof(toolCall.name));
    }
    if (const char *piece = call["function"]["arguments"]) {
      strlcat(toolCall.arguments, piece, sizeof(toolCall.arguments));
    }
  }
}

void handleLine() {
  doc.clear();
  line[lineLength] = 0;
  if (lineLength > 0 && line[lineLength - 1] == '\r') {
    line[--lineLength] = 0;
  }
  if (lineLength < 6 || strncmp(line, "data: ", 6) != 0) {
    return;
  }
  if (deserializeJson(doc, line + 6) != DeserializationError::Ok) {
    return;
  }

  JsonObject delta = doc["choices"][0]["delta"];
  if (const char *base64 = delta["audio"]["data"]) {
    handleAudio(base64);
  }
  if (const char *piece = delta["audio"]["transcript"]) {
    transcriptSeen = true;
    Serial.print(piece);
  }
  if (const char *piece = delta["content"]) {
    handleText(piece);
  }
  if (delta["tool_calls"].is<JsonArray>()) {
    handleToolCalls(delta["tool_calls"].as<JsonArray>());
  }
}

void resetParser() {
  lineLength = 0;
  lineDropped = false;
  overflowReported = false;
  memset(&toolCall, 0, sizeof(toolCall));
  text[0] = 0;
  audioSeen = false;
  transcriptSeen = false;
  textSeen = false;
  doc.clear();
}

}

bool parserBegin() {
  line = (char *)ps_malloc(eventBytes);
  pcm = (uint8_t *)ps_malloc(decodedBytes);
  return line != nullptr && pcm != nullptr;
}

SseSink::SseSink() { resetParser(); }

size_t SseSink::write(const uint8_t *data, size_t length) {
  if (playbackFailed() || line == nullptr) {
    return 0;
  }
  for (size_t i = 0; i < length; i++) {
    if (headLength < sizeof(head) - 1) {
      head[headLength++] = (char)data[i];
    }
    if (data[i] == '\n') {
      if (lineDropped) {
        lineDropped = false;
      } else {
        handleLine();
      }
      lineLength = 0;
      continue;
    }
    if (lineLength < eventBytes - 1) {
      line[lineLength++] = (char)data[i];
    } else if (!lineDropped) {
      lineDropped = true;
      if (!overflowReported) {
        overflowReported = true;
        Serial.printf("  event longer than %u bytes, skipped\n",
                      (unsigned)eventBytes);
      }
    }
  }
  return playbackFailed() ? 0 : length;
}

bool parserSawAudio() { return audioSeen; }

bool parserSawTranscript() { return transcriptSeen; }

bool parserSawText() { return textSeen; }

bool parserSawToolCall() { return toolCall.name[0] != 0; }

const char *parserText() { return text; }

void parserReset() { resetParser(); }

bool parserToolCall(ToolCall &call) {
  if (toolCall.name[0] == 0) {
    return false;
  }
  call = toolCall;
  return true;
}
