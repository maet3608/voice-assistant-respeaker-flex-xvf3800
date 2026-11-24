# voice-assistant-respeaker-flex-xvf3800

A voice assistant on the Seeed reSpeaker Flex XVF3800 with a XIAO ESP32S3 as host.
Press a button, ask a question, and the answer comes back as speech through the speaker of the board.
The question is recorded by the four microphones of the array board, sent to OpenAI `gpt-audio-1.5` as a WAV file, and the spoken answer is played back through the TLV320AIC3104 codec.

This repository holds the two finished sketches of the project.
The first one is a single blocking sketch and the second one is split into modules, streams the answer and offers a function to the model.

## What Is in This Repository

| Folder | Sketch | Behaviour |
| --- | --- | --- |
| `voice-assistant` | `voice-assistant.ino` | One request per turn. Waits for the complete WAV answer, then plays it. |
| `voice-assistant-streaming` | `voice-assistant-streaming.ino` plus helper modules | Reads server sent events and plays the audio while the answer is still arriving. Offers the `blink_led` function to the model. |

Both sketches fit next to each other, so you can open either folder in the Arduino IDE and upload it.

## How a Turn Works

The XIAO is the I2S slave of the XVF3800, so the voice processor drives the bit clock and the word clock of the 48 kHz, 2 channel, 32 bit bus.
The processed microphone audio arrives on data line 1, which the sketch decimates from 48 kHz to 16 kHz mono.

A turn starts when you press the button between D0 and GND.
The built-in LED lights up as a recording indicator and the sketch records until the talker stops, at most ten seconds.
The recording is wrapped in a WAV header in PSRAM and base64 encoded into a JSON request, because the API takes audio as inline data.
Then the sketch posts the conversation to the chat completions endpoint and receives the answer.

In the first sketch the answer arrives as one WAV file at 24 kHz, which is resampled to the 48 kHz bus and played through the codec.
In the streaming sketch the answer arrives as server sent events, and a separate FreeRTOS task moves the decoded samples into a ring buffer while the network task keeps reading.

Function calling needs two requests, because a request that offers a function never returns audio.
The streaming sketch therefore asks for text only first, runs `blink_led` when the model calls it, and then sends the conversation once more with the result and with audio requested.
One call per question is handled and there is no loop, so a confused model cannot start an endless chain of calls.

## Hardware Requirements

### The Board

You need a reSpeaker Flex XVF3800 core board with a Seeed Studio XIAO ESP32S3 on the castellated footprint.
Seeed sells the combination ready to use, and also as a bare board where you solder the XIAO yourself with a fine tip iron.
The package includes the 24 pin FPC cable and one microphone array board.
Pick the circular array for 360 degree pickup in a room, or the linear array for a device that faces one direction.

The core board carries the XMOS XVF3800 voice processor, the TLV320AIC3104 codec, a TPA3139D2 class D amplifier and all connectors.
The array board carries the four PDM microphones and connects with the 0.5 mm pitch FPC cable, so the microphones can sit where the sound is.

### Speaker or Headphones

A 4 ohm speaker with up to 10 W connects to the upright JST connector of the core board.
A hard wired speaker can also be soldered to the pad at the back.
Alternatively you plug headphones into the 3.5 mm jack, which has priority, so no sound leaves the speaker connector while headphones are inserted.
Headphones are the safer choice for the first test, because they keep the room quiet for the echo cancellation.

### Push Button and Wires

Both sketches need one momentary push button between pin D0 of the XIAO and GND.
The internal pull-up of the ESP32S3 is enabled in the sketch, so no resistor is needed and two jumper wires are enough.

### Power and Cables

Two USB-C data cables are useful.
The cable in the XIAO port carries the sketch uploads, the serial output and the 5 V supply.
The cable in the core board port next to the RST button reaches the XVF3800 for firmware updates.
Charge only cables will not work for either job.

Power normally comes in over a host port at 5 V, which is enough for the board and the speaker.
The external terminal is optional, takes 12 V and supports the full 10 W speaker load.

## Wiring

Most of the wiring is already done on the core board.
Only the button is added by hand.

| XIAO pin | GPIO | Signal | Direction |
| --- | --- | --- | --- |
| D9 | GPIO8 | I2S BCLK | XVF3800 to XIAO |
| D8 | GPIO7 | I2S LRCLK | XVF3800 to XIAO |
| D7 | GPIO44 | I2S DATA0, playback and echo reference | XIAO to XVF3800 |
| D6 | GPIO43 | I2S DATA1, processed microphone audio | XVF3800 to XIAO |
| D4 | GPIO5 | I2C SDA to the codec and the XVF3800 | Bidirectional |
| D5 | GPIO6 | I2C SCL to the codec and the XVF3800 | XIAO to board |
| D0 | GPIO1 | Push button to GND, active low | Input with pull-up |
| Built-in | GPIO21 | User LED, active low, doubles as recording indicator | Output |

The codec sits at I2C address `0x18` and is switched to line and headphone output with a volume of -12 dB at start up.

## Software Requirements

### Arduino IDE and Core

Use Arduino IDE 2.x with the ESP32 board package version 4.0.0 from the Boards Manager.
Select `XIAO_ESP32S3` as the board.

### Libraries

Install these libraries with the Library Manager.

| Library | Version | Used for |
| --- | --- | --- |
| [audio-tools](https://github.com/pschatzmann/arduino-audio-tools) | 1.2.6 | I2S streams, WAV writing, sample rate conversion |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.4.3 | Building the request and reading the streamed events |

The remaining includes come with the ESP32 core, namely `WiFi`, `HTTPClient`, `WiFiClientSecure` and `mbedtls`.

### Tool Settings

Two settings in the Tools menu are mandatory.
Without them the sketch either allocates no PSRAM or prints nothing to the Serial Monitor.

| Setting | Value |
| --- | --- |
| USB CDC On Boot | Enabled |
| PSRAM | OPI PSRAM |
| Partition Scheme | default, the sketch uses about 34 percent of 3.3 MB |

### Compiling

The sketches use about 1.14 MB of flash and 58 to 63 kB of static RAM.
With `arduino-cli` a build looks like this.

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:CDCOnBoot=default,PSRAM=opi voice-assistant-streaming
```

In this core the value `CDCOnBoot=default` is the Enabled setting, and `PSRAM` is disabled unless you ask for `PSRAM=opi`.

## The XVF3800 Firmware

The sketches expect the I2S build of the XVF3800 firmware, where the voice processor is the I2S master for a microcontroller host.
A Flex with the XIAO already fitted usually ships with that build and works right away.

If the board still runs the USB build, it shows up as a sound card on your computer and the XIAO reads silence forever.
In that case flash the I2S firmware with `dfu-util` over the USB-C port of the core board, not over the XIAO port.
The file for the circular array is `respeaker_flex_i2s_c16k2ch_v1.0.4.bin`, and the same folder holds the builds for the linear array, where the name starts with `l` instead of `c`.
Both sit in the [reSpeaker_Flex repository](https://github.com/respeaker/reSpeaker_Flex).
The [Seeed wiki](https://wiki.seeedstudio.com/respeaker_flex_introduction/) describes the flashing steps in detail.

## Getting Started

1. Clone this repository or download it as a ZIP file.
2. Open `voice-assistant/credentials.h` or `voice-assistant-streaming/credentials.h` and replace the three placeholders with your network name, your WiFi password and your OpenAI API key.
3. Connect the FPC cable, the button and your speaker or headphones.
4. Plug a USB-C cable into the XIAO, select the board and the two tool settings above, then upload the sketch.
5. Open the Serial Monitor at 115200 baud and wait for the ready message.
6. Press the button, ask your question, and listen to the answer.

The ESP32S3 only has a 2.4 GHz radio, so the access point must offer a 2.4 GHz network.

A serial session of the streaming sketch looks like this after the start up lines with the PSRAM size and the buffer sizes.

```text
Ready, press the D0 button and ask a question
Listening, ask your question
Done, press the D0 button for the next question
```

## Project Structure

### voice-assistant

This sketch is a single file and keeps everything in one place, which makes it the easier one to read.

| File | Contents |
| --- | --- |
| `voice-assistant.ino` | Recording, request, playback and the main loop |
| `credentials.h` | Network name, password and API key |

### voice-assistant-streaming

This sketch is split by job, so the streaming and the audio plumbing stay readable.

| File | Contents |
| --- | --- |
| `voice-assistant-streaming.ino` | One turn, `setup()` and `loop()` |
| `config.h` | Pins, API strings, sizes and recording behaviour |
| `credentials.h` | Network name, password and API key |
| `audio.h` / `audio.cpp` | I2S bus, recording, WAV header, ring buffer and playback task |
| `hardware.h` / `hardware.cpp` | WiFi, codec registers, LED and reset line |
| `request.h` / `request.cpp` | JSON request, base64 audio and HTTPS POST |
| `parser.h` / `parser.cpp` | Server sent events, audio deltas and function calls |
| `tools.h` / `tools.cpp` | The `blink_led` function and the tool table |
| `assistant.h` / `assistant.cpp` | The streaming turn with its decision request |

## Resources

The hardware is documented by Seeed in the [reSpeaker Flex wiki](https://wiki.seeedstudio.com/respeaker_flex_introduction/), and the firmware and host examples live in the [reSpeaker_Flex repository](https://github.com/respeaker/reSpeaker_Flex).
For the XIAO ESP32S3 have a look at [Getting Started with XIAO ESP32-S3 and Arduino IDE](https://www.makerguides.com/getting-started-with-xiao-esp32-s3-and-arduino-ide/).
More tutorials for Arduino and ESP32 are on [www.makerguides.com](https://www.makerguides.com/).

## License

This project is released under the Apache License 2.0, see [LICENSE](LICENSE).
The libraries keep their own licenses, `audio-tools` is GPL-3.0 and `ArduinoJson` is MIT.
