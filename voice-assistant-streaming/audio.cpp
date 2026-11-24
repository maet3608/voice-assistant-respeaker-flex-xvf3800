#include "audio.h"

#include "AudioTools.h"
#include "config.h"
#include "freertos/stream_buffer.h"
#include "hardware.h"

namespace {

AudioInfo busInfo(busRate, busChannels, busBits);
I2SStream i2s;

uint8_t *wavBuffer = nullptr;
int32_t recordPeak = 0;

int32_t readChunk[chunkBytes / sizeof(int32_t)];
int32_t playChunk[chunkBytes / sizeof(int32_t)];

constexpr size_t framesPerChunk = chunkBytes / (busChannels * sizeof(int32_t));

StaticStreamBuffer_t ringStruct;
uint8_t *ringStore = nullptr;
StreamBufferHandle_t ring = nullptr;

volatile bool playing = false;
volatile bool playFailed = false;
volatile bool finishing = false;
volatile bool taskRunning = false;
uint32_t requestStartMs = 0;
size_t playFrames = 0;
volatile size_t playedFrames = 0;
int32_t previousSample = 0;
bool havePreviousSample = false;
uint8_t pendingByte = 0;
bool havePendingByte = false;
uint8_t playBytes[chunkBytes];

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

}

namespace {

void flushPlayChunk() {
  if (playFrames == 0) {
    return;
  }
  size_t frames = playFrames;
  playFrames = 0;
  if (i2s.write((uint8_t *)playChunk, frames * busChannels * sizeof(int32_t)) ==
      0) {
    playFailed = true;
  }
}

void addFrame(int32_t value) {
  int32_t scaled = (int32_t)((uint32_t)value << 16);
  // the XVF3800 plays the left channel, so both slots carry the answer
  playChunk[playFrames * busChannels] = scaled;
  playChunk[playFrames * busChannels + 1] = scaled;
  playedFrames++;
  if (++playFrames == framesPerChunk) {
    flushPlayChunk();
  }
}

void resample(int16_t sample) {
  if (!havePreviousSample) {
    previousSample = sample;
    havePreviousSample = true;
    return;
  }
  addFrame(previousSample);
  addFrame((previousSample + (int32_t)sample) / 2);
  previousSample = sample;
}

void playTask(void *) {
  for (;;) {
    size_t received = xStreamBufferReceive(ring, playBytes, sizeof(playBytes),
                                           pdMS_TO_TICKS(20));
    if (received == 0) {
      if (finishing) {
        break;
      }
      continue;
    }
    for (size_t i = 0; i < received; i++) {
      if (!havePendingByte) {
        pendingByte = playBytes[i];
        havePendingByte = true;
        continue;
      }
      int16_t sample = (int16_t)(pendingByte | (playBytes[i] << 8));
      havePendingByte = false;
      resample(sample);
    }
  }
  flushPlayChunk();
  taskRunning = false;
  vTaskDelete(nullptr);
}

bool startPlayback() {
  setCodecVolumeDb(codecVolumeDb);
  if (!startI2S(TX_MODE)) {
    return false;
  }
  playing = true;
  finishing = false;
  havePreviousSample = false;
  havePendingByte = false;
  taskRunning = true;
  if (xTaskCreate(playTask, "play", playTaskStackBytes, nullptr, playTaskPriority,
                  nullptr) != pdPASS) {
    taskRunning = false;
    playing = false;
    Serial.println("  playback task failed");
    return false;
  }
  Serial.printf("  first audio after %lu ms\n",
                (unsigned long)(millis() - requestStartMs));
  return true;
}

}

bool audioBegin() {
  wavBuffer = (uint8_t *)ps_malloc(maxWavBytes);
  ringStore = (uint8_t *)ps_malloc(ringBytes + 1);
  if (ringStore != nullptr) {
    ring = xStreamBufferCreateStatic(ringBytes, 1, ringStore, &ringStruct);
  }
  return wavBuffer != nullptr && ring != nullptr;
}

const uint8_t *recordedWav() { return wavBuffer; }

int32_t lastRecordPeak() { return recordPeak; }

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
                dataBytes / 2.0f / recordRate, (unsigned long)(millis() - start),
                speechDetected ? "" : ", no speech");

  return speechDetected ? 44 + dataBytes : 0;
}

void requestStarted() { requestStartMs = millis(); }

void queueSamples(const uint8_t *data, size_t length) {
  if (!playing && !startPlayback()) {
    playFailed = true;
    return;
  }
  while (length > 0 && !playFailed) {
    size_t sent = xStreamBufferSend(ring, data, length, pdMS_TO_TICKS(500));
    data += sent;
    length -= sent;
  }
}

bool playbackFailed() { return playFailed; }

void endPlayback() {
  if (!playing) {
    return;
  }
  finishing = true;
  uint32_t start = millis();
  while (taskRunning && millis() - start < 3000) {
    delay(1);
  }
  delay(100);
  i2s.end();
  playing = false;
}

float playedSeconds() { return playedFrames / (float)busRate; }
