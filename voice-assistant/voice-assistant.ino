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

#include "ArduinoJson.h"
#include "AudioTools.h"
#include "HTTPClient.h"
#include "credentials.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "Wire.h"
#include <mbedtls/base64.h>

const char *apiUrl = "https://api.openai.com/v1/chat/completions";
const char *audioModel = "gpt-audio-1.5";
const char *audioVoice = "alloy";
const char *systemPrompt =
  "You are a friendly voice assistant. Answer in at most 40 "
  "words. Your answer is spoken aloud, so use plain sentences without lists, "
  "markdown or emoji.";
const char *userPrompt = "Answer the question in this recording.";

const int busRate = 48000;
const int busChannels = 2;
const int busBits = 32;
const int micChannel = 1; // the processed microphone audio is in slot 1

const uint32_t recordRate = 16000;
const int decimation = busRate / recordRate; // 3

const size_t chunkBytes = 4096;

const int pinBck = 8;           // D9 I2S_BCLK
const int pinWs = 7;            // D8 I2S_LRCK
const int pinDataOut = 44;      // D7 I2S_DATA0 to the XVF3800
const int pinDataIn = 43;       // D6 I2S_DATA1 from the XVF3800
const int pinLed = LED_BUILTIN; // GPIO21, active low
const int pinButton = 1;        // D0/GPIO1 to GND, active low, pulled up

const int i2cSda = 5; // D4
const int i2cScl = 6; // D5

const uint8_t aic3104Address = 0x18;
const uint8_t aic3104PageCtrl = 0x00;
const uint8_t aic3104LeftDac = 0x2B;
const uint8_t aic3104RightDac = 0x2C;
const uint8_t aic3104HpLeft = 0x33;
const uint8_t aic3104HpRight = 0x41;
const uint8_t aic3104LineLeft = 0x56;
const uint8_t aic3104LineRight = 0x5D;

const float codecVolumeDb = -12.0f;

const uint32_t maxRecordMs = 10000;
const uint32_t listenTimeoutMs = 6000;  // give up when nothing is said
const uint32_t trailingSilenceMs = 900; // stop after this much silence
const int16_t speechLevel = 600;        // about -35 dBFS

const size_t audioCapacity = (size_t)recordRate * (maxRecordMs / 1000) * 2;
const size_t maxWavBytes = 44 + audioCapacity;
const size_t maxBase64Bytes = (maxWavBytes / 3 + 1) * 4 + 4;
const size_t requestCapacity = maxBase64Bytes + 4096;
const size_t responseCapacity = 1536 * 1024;
const size_t pcmCapacity = responseCapacity / 4 * 3 + 4096;

AudioInfo busInfo(busRate, busChannels, busBits);
I2SStream i2s;

uint8_t *wavBuffer = nullptr;
char *requestBuffer = nullptr;
char *responseBuffer = nullptr;
uint8_t *pcmBuffer = nullptr;

int32_t recordPeak = 0;

static int32_t readChunk[chunkBytes / sizeof(int32_t)];
static int32_t playChunk[chunkBytes / sizeof(int32_t)];

struct WavInfo {
  uint16_t format;
  uint16_t channels;
  uint32_t sampleRate;
  uint16_t bits;
  const uint8_t *data;
  size_t dataBytes;
};

void setLed(bool on) { digitalWrite(pinLed, on ? LOW : HIGH); }

uint8_t aic3104Write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(aic3104Address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission();
}

void setCodecVolumeDb(float db) {
  int steps = constrain((int)lroundf(-db * 2.0f), 0, 127);
  aic3104Write(aic3104LeftDac, (uint8_t)steps);
  aic3104Write(aic3104RightDac, (uint8_t)steps);
}

void setupCodec() {
  Wire.begin(i2cSda, i2cScl);

  uint8_t failures = 0;
  failures += aic3104Write(aic3104PageCtrl, 0x00) != 0;
  failures += aic3104Write(aic3104HpLeft, 0x0D) != 0;
  failures += aic3104Write(aic3104HpRight, 0x0D) != 0;
  failures += aic3104Write(aic3104LineLeft, 0x0B) != 0;
  failures += aic3104Write(aic3104LineRight, 0x0B) != 0;
  setCodecVolumeDb(codecVolumeDb);

  if (failures > 0) {
    Serial.printf("AIC3104: %u I2C writes failed, check SDA/SCL\n", failures);
  } else {
    Serial.printf("AIC3104: volume %.1f dB\n", codecVolumeDb);
  }
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid, wifiPassword);

  Serial.printf("Connecting to %s\n", wifiSsid);
  if (WiFi.waitForConnectResult(15000) != WL_CONNECTED) {
    Serial.println("WiFi failed, check ssid and password");
    return false;
  }
  Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool startI2S(RxTxMode mode) {
  I2SConfig cfg = i2s.defaultConfig(mode);
  cfg.copyFrom(busInfo);
  cfg.pin_bck = pinBck;
  cfg.pin_ws = pinWs;
  cfg.buffer_size = 4096;
  cfg.buffer_count = 8;
  cfg.is_master = false;

  if (mode == RX_MODE) {
    cfg.pin_data = pinDataIn;
    cfg.pin_data_rx = pinDataIn;
  } else {
    cfg.pin_data = pinDataOut;
  }

  if (!i2s.begin(cfg)) {
    Serial.printf("I2S %s start failed\n", mode == RX_MODE ? "RX" : "TX");
    return false;
  }
  return true;
}

uint16_t readUint16(const uint8_t *p) { return p[0] | (p[1] << 8); }

uint32_t readUint32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void writeWavHeader(uint8_t *buffer, uint32_t dataBytes) {
  const uint16_t pcm = 1;
  const uint16_t mono = 1;
  const uint16_t bits = 16;
  const uint16_t blockAlign = 2;
  const uint32_t fmtBytes = 16;
  const uint32_t byteRate = recordRate * blockAlign;
  const uint32_t riffBytes = 36 + dataBytes;

  memcpy(buffer, "RIFF", 4);
  memcpy(buffer + 4, &riffBytes, 4);
  memcpy(buffer + 8, "WAVEfmt ", 8);
  memcpy(buffer + 16, &fmtBytes, 4);
  memcpy(buffer + 20, &pcm, 2);
  memcpy(buffer + 22, &mono, 2);
  memcpy(buffer + 24, &recordRate, 4);
  memcpy(buffer + 28, &byteRate, 4);
  memcpy(buffer + 32, &blockAlign, 2);
  memcpy(buffer + 34, &bits, 2);
  memcpy(buffer + 36, "data", 4);
  memcpy(buffer + 40, &dataBytes, 4);
}

size_t recordSpeech() {
  bool speechDetected = false;
  recordPeak = 0;
  if (!startI2S(RX_MODE)) {
    return 0;
  }

  int16_t *samples = (int16_t *)(wavBuffer + 44);
  const size_t maxSamples = audioCapacity / sizeof(int16_t);
  size_t sampleCount = 0;

  int64_t sum = 0;
  int inGroup = 0;
  uint32_t start = millis();
  uint32_t lastVoice = start;
  uint32_t nextReport = start + 1000;

  while (millis() - start < maxRecordMs) {
    size_t received = i2s.readBytes((uint8_t *)readChunk, chunkBytes);
    if (received == 0) {
      break;
    }
    size_t framesRead = received / (busChannels * sizeof(int32_t));

    int32_t peak = 0;
    for (size_t i = 0; i < framesRead && sampleCount < maxSamples; i++) {
      sum += readChunk[i * busChannels + micChannel];
      if (++inGroup < decimation) {
        continue;
      }
      int16_t sample = (int16_t)((sum / decimation) >> 16);
      sum = 0;
      inGroup = 0;
      samples[sampleCount++] = sample;
      int32_t level = sample < 0 ? -sample : sample;
      if (peak < level) {
        peak = level;
      }
    }

    uint32_t now = millis();
    if (recordPeak < peak) {
      recordPeak = peak;
    }
    if (speechLevel <= peak) {
      speechDetected = true;
      lastVoice = now;
    }
    if (nextReport <= now) {
      Serial.printf("  peak %ld, %u samples\n", (long)peak,
                    (unsigned)sampleCount);
      nextReport = now + 1000;
    }
    if (sampleCount >= maxSamples) {
      Serial.println("  record buffer full");
      break;
    }
    if (speechDetected && trailingSilenceMs <= now - lastVoice) {
      break;
    }
    if (!speechDetected && listenTimeoutMs <= now - start) {
      break;
    }
  }

  i2s.end();

  size_t dataBytes = sampleCount * sizeof(int16_t);
  writeWavHeader(wavBuffer, dataBytes);
  Serial.printf("  captured %u bytes (%.1f s) in %lu ms%s\n", (unsigned)dataBytes,
                dataBytes / 2.0f / recordRate, millis() - start,
                speechDetected ? "" : ", no speech");

  return speechDetected ? 44 + dataBytes : 0;
}

// ArduinoJson cuts strings at 65535 characters, so the audio goes in via a placeholder
const char audioPlaceholder[] = "@@AUDIO@@";

size_t buildRequest(size_t wavBytes) {
  JsonDocument doc;

  doc["model"] = audioModel;
  JsonArray modalities = doc["modalities"].to<JsonArray>();
  modalities.add("text");
  modalities.add("audio");
  doc["audio"]["voice"] = audioVoice;
  doc["audio"]["format"] = "wav";
  doc["store"] = false;

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
                                     wavBuffer, wavBytes);
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

class ResponseSink : public Stream {
public:
  ResponseSink(char *buffer, size_t capacity)
    : buffer(buffer), capacity(capacity) {}

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t *data, size_t length) override {
    if (capacity <= used + length) {
      overflow = true;
      return 0;
    }
    memcpy(buffer + used, data, length);
    used += length;
    return length;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t used = 0;
  bool overflow = false;

private:
  char *buffer;
  size_t capacity;
};

bool postRequest(size_t length, size_t &responseLength) {
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
  int status = http.POST((uint8_t *)requestBuffer, length);
  if (status != HTTP_CODE_OK) {
    Serial.printf("  HTTP %d\n", status);
    String body = http.getString();
    if (body.length() > 0) {
      Serial.println(body.substring(0, 400));
    }
    http.end();
    return false;
  }

  ResponseSink sink(responseBuffer, responseCapacity);
  int written = http.writeToStream(&sink);
  http.end();

  if (sink.overflow) {
    Serial.printf("  response does not fit into %u bytes\n",
                  (unsigned)responseCapacity);
    return false;
  }
  if (written < 0) {
    Serial.printf("  response read failed: %s\n",
                  HTTPClient::errorToString(written).c_str());
    return false;
  }

  responseLength = sink.used;
  responseBuffer[responseLength] = 0;
  return true;
}

bool parseWav(const uint8_t *buffer, size_t length, WavInfo &wav) {
  if (length < 44 || memcmp(buffer, "RIFF", 4) != 0 ||
      memcmp(buffer + 8, "WAVE", 4) != 0) {
    Serial.println("  not a RIFF/WAVE file");
    return false;
  }

  wav.format = readUint16(buffer + 20);
  wav.channels = readUint16(buffer + 22);
  wav.sampleRate = readUint32(buffer + 24);
  wav.bits = readUint16(buffer + 34);
  wav.data = buffer + 44;
  wav.dataBytes = length - 44;
  return 0 < wav.dataBytes;
}

void playWav(const uint8_t *buffer, size_t length) {
  WavInfo wav = {};
  if (!parseWav(buffer, length, wav)) {
    return;
  }
  Serial.printf("  WAV: %u Hz, %u ch, %u bit, %u bytes\n",
                (unsigned)wav.sampleRate, (unsigned)wav.channels,
                (unsigned)wav.bits, (unsigned)wav.dataBytes);

  if (wav.format != 1 || wav.bits != 16 || wav.channels == 0 ||
      wav.sampleRate < 8000 || 192000 < wav.sampleRate) {
    Serial.println("  only 16 bit PCM can be played");
    return;
  }

  const size_t inFrameBytes = wav.channels * sizeof(int16_t);
  const size_t inFrames = wav.dataBytes / inFrameBytes;
  if (inFrames < 2) {
    Serial.println("  answer too short to play");
    return;
  }

  const uint32_t step =
    (uint32_t)(((uint64_t)wav.sampleRate << 16) / (uint64_t)busRate);
  const size_t framesPerChunk = chunkBytes / (busChannels * sizeof(int32_t));

  setCodecVolumeDb(codecVolumeDb);
  if (!startI2S(TX_MODE)) {
    return;
  }

  size_t index = 0;
  uint32_t fraction = 0;
  size_t writtenBytes = 0;
  uint32_t start = millis();

  while (true) {
    size_t frames = 0;
    while (frames < framesPerChunk) {
      if (inFrames <= index + 1) {
        break;
      }
      const uint8_t *low = wav.data + index * inFrameBytes;
      const uint8_t *high = low + inFrameBytes;
      int32_t a = (int16_t)readUint16(low);
      int32_t b = (int16_t)readUint16(high);
      int32_t value = a + (int32_t)(((int64_t)(b - a) * fraction) >> 16);
      int32_t scaled = (int32_t)((uint32_t)value << 16);
      // the XVF3800 plays the left channel, so both slots carry the answer
      playChunk[frames * busChannels] = scaled;
      playChunk[frames * busChannels + 1] = scaled;
      frames++;
      fraction += step;
      index += fraction >> 16;
      fraction &= 0xFFFF;
    }

    if (frames == 0) {
      break;
    }
    size_t bytes = frames * busChannels * sizeof(int32_t);
    if (i2s.write((uint8_t *)playChunk, bytes) == 0) {
      Serial.println("  I2S TX write failed");
      break;
    }
    writtenBytes += bytes;
  }

  delay(100);
  i2s.end();
  Serial.printf("  played %u bytes (%.1f s) in %lu ms\n", (unsigned)writtenBytes,
                inFrames / (float)wav.sampleRate, millis() - start);
}

bool findAudioData(char *body, const char *&data, size_t &dataLength) {
  const char *audio = strstr(body, "\"audio\"");
  const char *key = audio ? strstr(audio, "\"data\"") : nullptr;
  const char *colon = key ? strchr(key, ':') : nullptr;
  const char *start = colon ? strchr(colon, '"') : nullptr;
  const char *end = start ? strchr(start + 1, '"') : nullptr;
  if (end == nullptr || end == start + 1) {
    return false;
  }
  data = start + 1;
  dataLength = end - data;
  return true;
}

void playResponse(size_t length) {
  JsonDocument filter;
  filter["error"]["message"] = true;
  filter["choices"][0]["message"]["audio"]["transcript"] = true;
  filter["choices"][0]["message"]["content"] = true;

  JsonDocument doc;
  DeserializationError error =
    deserializeJson(doc, responseBuffer, length,
                    DeserializationOption::Filter(filter));
  if (error) {
    Serial.printf("  JSON error: %s\n", error.c_str());
    return;
  }

  if (doc["error"].is<JsonObject>()) {
    Serial.printf("  API error: %s\n",
                  (const char *)(doc["error"]["message"] | "unknown"));
    return;
  }

  const char *transcript =
    doc["choices"][0]["message"]["audio"]["transcript"] | "";
  if (*transcript != 0) {
    Serial.printf("  answer: %s\n", transcript);
  }

  const char *data = nullptr;
  size_t dataLength = 0;
  if (!findAudioData(responseBuffer, data, dataLength)) {
    Serial.printf("  no audio in the response: %s\n",
                  (const char *)(doc["choices"][0]["message"]["content"] | ""));
    return;
  }

  size_t decoded = 0;
  int result = mbedtls_base64_decode(pcmBuffer, pcmCapacity, &decoded,
                                     (const unsigned char *)data, dataLength);
  if (result != 0) {
    Serial.printf("  base64 decode failed: %d, %u characters for %u bytes\n",
                  result, (unsigned)dataLength, (unsigned)pcmCapacity);
    return;
  }
  playWav(pcmBuffer, decoded);
}

void doTurn() {
  Serial.println("\nListening, ask your question");

  setLed(true);
  size_t wavBytes = recordSpeech();
  setLed(false);
  if (wavBytes == 0) {
    Serial.printf("Nothing to send, loudest peak %ld against threshold %d\n",
                  (long)recordPeak, speechLevel);
    Serial.println("If you did speak, lower speechLevel and upload again");
    return;
  }

  size_t requestBytes = buildRequest(wavBytes);
  if (requestBytes == 0) {
    return;
  }

  uint32_t start = millis();
  size_t responseBytes = 0;
  if (!postRequest(requestBytes, responseBytes)) {
    return;
  }
  Serial.printf("  response: %u bytes in %lu ms\n", (unsigned)responseBytes,
                millis() - start);

  playResponse(responseBytes);
  Serial.println("Done, press the D0 button for the next question");
}

bool allocateBuffers() {
  wavBuffer = (uint8_t *)ps_malloc(maxWavBytes);
  requestBuffer = (char *)ps_malloc(requestCapacity);
  responseBuffer = (char *)ps_malloc(responseCapacity);
  pcmBuffer = (uint8_t *)ps_malloc(pcmCapacity);

  if (!wavBuffer || !requestBuffer || !responseBuffer || !pcmBuffer) {
    Serial.println("PSRAM allocation failed");
    Serial.println("Set PSRAM to OPI PSRAM in the Tools menu and upload again");
    return false;
  }

  Serial.printf("buffers: %u + %u + %u + %u bytes, %u bytes free\n",
                (unsigned)maxWavBytes, (unsigned)requestCapacity,
                (unsigned)responseCapacity, (unsigned)pcmCapacity,
                (unsigned)ESP.getFreePsram());
  return true;
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
  if (!allocateBuffers()) {
    return;
  }

  setupCodec();
  if (!connectWiFi()) {
    return;
  }

  delay(1000);
  Serial.println("Ready, press the D0 button and ask a question");
}

void loop() {
  if (digitalRead(pinButton) == LOW) {
    doTurn();
    delay(500);
  }
}
