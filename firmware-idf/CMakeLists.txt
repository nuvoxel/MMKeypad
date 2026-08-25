# MMKeypad — native ESP-IDF firmware (Phase 3 / intercom spike)
# Build:  idf.py set-target esp32s3 build
# Flash:  idf.py -p /dev/cu.usbmodem4101 flash monitor
cmake_minimum_required(VERSION 3.16)

# Open build: identity and OTA-apply are provided locally by main/
# (nv_identity_open.c, nv_ota_open.c) -- no external component tree.
# Point EXTRA_COMPONENT_DIRS at a directory only if you add out-of-tree
# components via NUVOXEL_COMPONENTS_DIR.
if(DEFINED ENV{NUVOXEL_COMPONENTS_DIR})
  set(EXTRA_COMPONENT_DIRS "$ENV{NUVOXEL_COMPONENTS_DIR}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmkeypad_idf)
