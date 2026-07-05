# If not stated otherwise in this file or this component's license file the
# following copyright and licenses apply:
#
# Copyright 2020 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(PLUGIN_NAME FrontPanel)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})
set(PLUGIN_IMPLEMENTATION ${MODULE_NAME}Implementation)

set(PLUGIN_FRONTPANEL_STARTUPORDER "" CACHE STRING "To configure startup order of FrontPanel plugin")

find_package(${NAMESPACE}Plugins REQUIRED)
find_package(${NAMESPACE}Helpers REQUIRED)

add_library(${MODULE_NAME} SHARED
        FrontPanel.cpp
        Module.cpp)

# ── DeviceSettings plugin COM-RPC integration ─────────────────────────────────
# Selects DS_COMRPC/ or DS_IARM/ folder for both the plugin implementation and
# the CFrontPanel helper, so no #ifdef USE_DEVICESETTING_PLUGIN exists inside
# any .cpp or .h file.
# ──────────────────────────────────────────────────────────────────────────────
option(USE_DEVICESETTING_PLUGIN
    "Enable COM-RPC integration with the DeviceSettings plugin for FrontPanel (replaces libds/IARM path)"
    ON)

if (USE_DEVICESETTING_PLUGIN)
    # ── COM-RPC path ──────────────────────────────────────────────────────────
    # DS_COMRPC/ — uses IDeviceSettingsFPD via DeviceSettingsClientHelper.
    # Does NOT link libds.so or libdshal-cli.so.
    add_library(${PLUGIN_IMPLEMENTATION} SHARED
            DS_COMRPC/FrontPanelImplementation.cpp
            Module.cpp
            ../helpers/DS_COMRPC/frontpanel.cpp)
    set_source_files_properties(
            DS_COMRPC/FrontPanelImplementation.cpp
            ../helpers/DS_COMRPC/frontpanel.cpp
        PROPERTIES COMPILE_FLAGS "-fexceptions")
    target_compile_definitions(${PLUGIN_IMPLEMENTATION} PRIVATE USE_DEVICESETTING_PLUGIN)
    target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE
            DS_COMRPC
            ../helpers/DS_COMRPC
            ../helpers)
    find_package(${NAMESPACE}Definitions REQUIRED)
    target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE
            ${NAMESPACE}Plugins::${NAMESPACE}Plugins
            ${NAMESPACE}Definitions::${NAMESPACE}Definitions
            ${NAMESPACE}::${NAMESPACE}Helpers)
    message(STATUS "FrontPanel: USE_DEVICESETTING_PLUGIN=ON — DS_COMRPC/ path (no libds)")
else()
    # ── IARM / libds path ─────────────────────────────────────────────────────
    # DS_IARM/ — uses device:: libds APIs directly via IARM bus.
    add_library(${PLUGIN_IMPLEMENTATION} SHARED
            DS_IARM/FrontPanelImplementation.cpp
            Module.cpp
            ../helpers/DS_IARM/frontpanel.cpp)
    set_source_files_properties(
            DS_IARM/FrontPanelImplementation.cpp
            ../helpers/DS_IARM/frontpanel.cpp
        PROPERTIES COMPILE_FLAGS "-fexceptions")
    target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE
            DS_IARM
            ../helpers/DS_IARM
            ../helpers)
    find_package(DS)
    if (DS_FOUND)
        find_package(IARMBus)
        add_definitions(-DDS_FOUND)
        target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${IARMBUS_INCLUDE_DIRS})
        target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${DS_INCLUDE_DIRS})
        target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE
                ${NAMESPACE}Plugins::${NAMESPACE}Plugins
                ${IARMBUS_LIBRARIES}
                ${DS_LIBRARIES}
                ${NAMESPACE}::${NAMESPACE}Helpers)
    else()
        target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE
                ${NAMESPACE}Plugins::${NAMESPACE}Plugins
                ${NAMESPACE}::${NAMESPACE}Helpers)
    endif()
    message(STATUS "FrontPanel: USE_DEVICESETTING_PLUGIN=OFF — DS_IARM/ path (libds/IARM)")
endif() # USE_DEVICESETTING_PLUGIN

set_target_properties(${MODULE_NAME} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

set_target_properties(${PLUGIN_IMPLEMENTATION} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_NAME=Plugin_${PLUGIN_NAME})
target_compile_definitions(${PLUGIN_IMPLEMENTATION} PRIVATE MODULE_NAME=Plugin_${PLUGIN_NAME})

# MODULE_NAME always links against Plugins (for the JSON-RPC proxy layer)
find_package(DS)
if (DS_FOUND)
    find_package(IARMBus)
    target_include_directories(${MODULE_NAME} PRIVATE ${IARMBUS_INCLUDE_DIRS})
    target_include_directories(${MODULE_NAME} PRIVATE ${DS_INCLUDE_DIRS})
endif()
target_include_directories(${MODULE_NAME} PRIVATE ../helpers)
target_link_libraries(${MODULE_NAME} PRIVATE
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        ${NAMESPACE}::${NAMESPACE}Helpers)

if (RDK_SERVICE_L2_TEST)
    find_library(TESTMOCKLIB_LIBRARIES NAMES TestMocklib)
    if (TESTMOCKLIB_LIBRARIES)
        message("linking mock libraries ${TESTMOCKLIB_LIBRARIES} library")
        target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE ${TESTMOCKLIB_LIBRARIES})
    else()
        message("Require ${TESTMOCKLIB_LIBRARIES} library")
    endif()
endif()

install(TARGETS ${MODULE_NAME}
        DESTINATION lib/${STORAGE_DIRECTORY}/plugins)
install(TARGETS ${PLUGIN_IMPLEMENTATION}
        DESTINATION lib/${STORAGE_DIRECTORY}/plugins)

write_config(${PLUGIN_NAME})
