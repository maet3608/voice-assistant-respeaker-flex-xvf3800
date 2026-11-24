#include "assistant.h"

#include "HTTPClient.h"
#include "WiFiClientSecure.h"
#include "audio.h"
#include "config.h"
#include "credentials.h"
#include "parser.h"
#include "request.h"

bool streamAnswer(size_t length, bool expectAudio) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  if (!http.begin(client, apiUrl)) {
    Serial.println("  https begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + apiKey);

  Serial.printf("  uploading %u bytes\n", (unsigned)length);
  requestStarted();
  uint32_t start = millis();
  int status = http.POST((uint8_t *)requestBody(), length);
  if (status != HTTP_CODE_OK) {
    Serial.printf("  HTTP %d\n", status);
    String body = http.getString();
    if (body.length() > 0) {
      Serial.println(body.substring(0, 400));
    }
    http.end();
    return false;
  }

  SseSink sink;
  int written = http.writeToStream(&sink);
  http.end();
  endPlayback();

  if (parserSawTranscript()) {
    Serial.println();
  }
  if (parserSawText()) {
    Serial.printf("\n  model: %s\n", parserText());
  }
  if (written < 0) {
    Serial.printf("  stream failed: %s\n",
                  HTTPClient::errorToString(written).c_str());
  }
  if (expectAudio && !parserSawAudio()) {
    Serial.printf("  no audio in the stream, %d body bytes, start of body:\n",
                  written);
    Serial.println(sink.bodyHead());
  }
  if (!expectAudio && !parserSawText() && !parserSawToolCall()) {
    Serial.printf("  neither text nor a call, %d body bytes, start of body:\n",
                  written);
    Serial.println(sink.bodyHead());
  }
  Serial.printf("  %d body bytes in %lu ms, %.1f s played\n", written,
                (unsigned long)(millis() - start), playedSeconds());
  return parserSawAudio();
}
