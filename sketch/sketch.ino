name: Build ESP32 Firmware

on: [push, workflow_dispatch]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout code
        uses: actions/checkout@v3

      - name: Setup Arduino CLI
        uses: arduino/setup-arduino-cli@v1

      - name: Install ESP32 Core
        run: |
          arduino-cli config init
          arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
          arduino-cli core update-index
          arduino-cli core install esp32:esp32

      - name: Compile Sketch
        run: |
          arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir ./build sketch

      - name: Upload Binary Artifact
        uses: actions/upload-artifact@v4
        with:
          name: esp32-firmware-bin
          path: build/*.bin
