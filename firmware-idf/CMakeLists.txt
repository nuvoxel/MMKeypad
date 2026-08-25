# MMKeypad — native ESP-IDF firmware (Phase 3 / intercom spike)
# Build:  idf.py set-target esp32s3 build
# Flash:  idf.py -p /dev/cu.usbmodem4101 flash monitor
cmake_minimum_required(VERSION 3.16)

# Shared nuvoxel device-plane client (identity / check-in / OTA / entitlement).
# Lives at the monorepo root: firmware/components. Override with
# NUVOXEL_COMPONENTS_DIR if this tree is checked out somewhere unusual.
# (firmware-idf/ -> apps/mmkeypad -> apps -> <repo root>/firmware/components)
if(DEFINED ENV{NUVOXEL_COMPONENTS_DIR})
  set(EXTRA_COMPONENT_DIRS "$ENV{NUVOXEL_COMPONENTS_DIR}")
else()
  set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../firmware/components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmkeypad_idf)
