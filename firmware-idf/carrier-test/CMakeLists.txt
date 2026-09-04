# MMKeypad — backbox-poe CARRIER diagnostic image.
# Separate IDF project so it can't disturb the shipping firmware in ../.
# Build:  source ~/esp/esp-idf/export.sh && cd firmware-idf/carrier-test && idf.py set-target esp32p4 && idf.py build
# Flash:  idf.py -p /dev/cu.wchusbserial* flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmkeypad_carrier_test)
