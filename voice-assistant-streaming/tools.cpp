#include "tools.h"

#include "ArduinoJson.h"
#include "config.h"
#include "hardware.h"

String runTool(const ToolCall &call) {
  JsonDocument args;
  if (deserializeJson(args, call.arguments) != DeserializationError::Ok) {
    Serial.println("  tool: the arguments are not JSON");
    return "The arguments were not valid JSON.";
  }

  if (strcmp(call.name, "blink_led") == 0) {
    int count = args["count"] | 0;
    if (count < 1) {
      Serial.println("  tool: blink_led without a usable count");
      return "The LED was not blinked, the count was missing.";
    }
    if (count > maxBlinkCount) {
      count = maxBlinkCount;
    }
    for (int i = 0; i < count; i++) {
      setLed(true);
      delay(blinkIntervalMs);
      setLed(false);
      delay(blinkIntervalMs);
    }
    Serial.printf("  tool: blink_led(%d)\n", count);
    return count == 1 ? String("The built-in LED blinked once.")
                      : String("The built-in LED blinked ") + count + " times.";
  }

  Serial.printf("  tool: unknown function %s\n", call.name);
  return "There is no such function.";
}
