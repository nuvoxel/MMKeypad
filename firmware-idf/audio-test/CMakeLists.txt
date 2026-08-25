# MMKeypad — standalone AUDIO BRING-UP / placement test image.
# Separate IDF project so it can't disturb the shipping firmware in ../.
# Build:  source ~/esp/esp-idf/export.sh && cd firmware-idf/audio-test && idf.py build
# Flash:  idf.py -p /dev/cu.usbmodem* flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmkeypad_audio_test)
