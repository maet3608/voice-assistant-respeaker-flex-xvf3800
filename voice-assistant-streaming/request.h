// Builds the JSON body with the recording inlined as base64.

#pragma once

#include <Arduino.h>
#include "parser.h"

bool requestBegin();

const char *requestBody();

size_t buildDecisionRequest(size_t wavBytes);

size_t buildAnswerRequest(size_t wavBytes, const ToolCall *toolCall = nullptr,
                          const char *toolResult = nullptr);
