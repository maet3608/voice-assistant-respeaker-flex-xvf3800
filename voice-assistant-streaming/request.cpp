#include "request.h"

#include "ArduinoJson.h"
#include "audio.h"
#include "config.h"
#include <mbedtls/base64.h>

namespace {

char *requestBuffer = nullptr;

// ArduinoJson caps string lengths at 65535 chars, so the base64 blob would turn
// into null. A placeholder is serialized instead and the blob is written into
// the room it reserved.
const char audioPlaceholder[] = "@@AUDIO@@";

void addTools(JsonDocument &doc) {
  JsonArray tools = doc["tools"].to<JsonArray>();
  JsonObject tool = tools.add<JsonObject>();
  tool["type"] = "function";

  JsonObject function = tool["function"].to<JsonObject>();
  function["name"] = "blink_led";
  function["description"] = "Blink the built-in LED of the board";

  JsonObject parameters = function["parameters"].to<JsonObject>();
  parameters["type"] = "object";
  JsonObject count = parameters["properties"]["count"].to<JsonObject>();
  count["type"] = "integer";
  count["description"] = "How many times the LED blinks, one blink is 500 ms on "
                         "and 500 ms off";
  parameters["required"].to<JsonArray>().add("count");
}

void addMessages(JsonDocument &doc, const ToolCall *toolCall,
                 const char *toolResult) {
  JsonArray messages = doc["messages"].to<JsonArray>();

  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] = systemPrompt;

  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  JsonArray parts = user["content"].to<JsonArray>();

  JsonObject text = parts.add<JsonObject>();
  text["type"] = "text";
  text["text"] = userPrompt;

  JsonObject audio = parts.add<JsonObject>();
  audio["type"] = "input_audio";
  audio["input_audio"]["format"] = "wav";
  audio["input_audio"]["data"] = audioPlaceholder;

  if (toolCall == nullptr) {
    return;
  }

  JsonObject assistant = messages.add<JsonObject>();
  assistant["role"] = "assistant";
  assistant["content"] = nullptr;
  JsonObject call = assistant["tool_calls"].to<JsonArray>().add<JsonObject>();
  call["id"] = toolCall->id;
  call["type"] = "function";
  call["function"]["name"] = toolCall->name;
  call["function"]["arguments"] = toolCall->arguments;

  JsonObject result = messages.add<JsonObject>();
  result["role"] = "tool";
  result["tool_call_id"] = toolCall->id;
  result["content"] = toolResult;
}

}

bool requestBegin() {
  requestBuffer = (char *)ps_malloc(requestCapacity);
  return requestBuffer != nullptr;
}

const char *requestBody() { return requestBuffer; }

namespace {

size_t build(size_t wavBytes, bool tools, bool spoken, const ToolCall *toolCall,
             const char *toolResult) {
  JsonDocument doc;

  doc["model"] = audioModel;
  JsonArray modalities = doc["modalities"].to<JsonArray>();
  modalities.add("text");
  if (spoken) {
    modalities.add("audio");
    doc["audio"]["voice"] = audioVoice;
    doc["audio"]["format"] = streamFormat;
  }
  doc["store"] = false;
  doc["stream"] = true;
  if (tools) {
    addTools(doc);
  }

  addMessages(doc, toolCall, toolResult);

  size_t length = serializeJson(doc, requestBuffer, requestCapacity);
  if (length == 0 || requestCapacity <= length) {
    Serial.println("request does not fit into requestBuffer");
    return 0;
  }

  char *mark = strstr(requestBuffer, audioPlaceholder);
  const size_t placeholderLength = strlen(audioPlaceholder);

  const size_t reserved = (wavBytes / 3 + 1) * 4 + 4;
  if (mark == nullptr || requestCapacity < length + reserved) {
    Serial.printf("no room for %u base64 bytes in %u\n", (unsigned)reserved,
                  (unsigned)requestCapacity);
    return 0;
  }

  const size_t offset = mark - requestBuffer;
  const size_t tailBytes = length - offset - placeholderLength + 1;
  memmove(mark + reserved, mark + placeholderLength, tailBytes);

  size_t encoded = 0;
  int result = mbedtls_base64_encode((unsigned char *)mark, reserved, &encoded,
                                     recordedWav(), wavBytes);
  if (result != 0 || encoded == 0) {
    Serial.printf("base64 encode failed: %d, %u bytes into %u\n", result,
                  (unsigned)wavBytes, (unsigned)reserved);
    return 0;
  }

  if (encoded < reserved) {
    memmove(mark + encoded, mark + reserved, tailBytes);
  }
  Serial.printf("  base64: %u characters for %u bytes\n", (unsigned)encoded,
                (unsigned)wavBytes);

  return length - placeholderLength + encoded;
}

}

size_t buildDecisionRequest(size_t wavBytes) {
  return build(wavBytes, true, false, nullptr, nullptr);
}

size_t buildAnswerRequest(size_t wavBytes, const ToolCall *toolCall,
                          const char *toolResult) {
  return build(wavBytes, false, true, toolCall, toolResult);
}
