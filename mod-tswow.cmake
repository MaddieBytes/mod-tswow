set(TSWOW_SOURCE_DIR "${TSWOW_SOURCE_DIR}" CACHE PATH "Path to the TSWoW source checkout")

if(NOT TSWOW_SOURCE_DIR)
  get_filename_component(TSWOW_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../tswow/tswow" ABSOLUTE)
endif()

set(TSWOW_CUSTOM_PACKET_DIR
  "${TSWOW_SOURCE_DIR}/misc/client-extensions/CustomPackets")

if(NOT EXISTS "${TSWOW_CUSTOM_PACKET_DIR}/CustomPacketBuffer.cpp")
  message(FATAL_ERROR
    "mod-tswow could not find TSWoW custom-packet sources at ${TSWOW_CUSTOM_PACKET_DIR}. "
    "Set TSWOW_SOURCE_DIR to the TSWoW repository root.")
endif()

target_sources(modules PRIVATE
  "${TSWOW_CUSTOM_PACKET_DIR}/CustomPacketBase.cpp"
  "${TSWOW_CUSTOM_PACKET_DIR}/CustomPacketBuffer.cpp"
  "${TSWOW_CUSTOM_PACKET_DIR}/CustomPacketChunk.cpp"
  "${TSWOW_CUSTOM_PACKET_DIR}/CustomPacketRead.cpp"
  "${TSWOW_CUSTOM_PACKET_DIR}/CustomPacketWrite.cpp")

target_include_directories(modules PRIVATE "${TSWOW_CUSTOM_PACKET_DIR}")

# The event-forwarding translation unit is intentionally centralized so module
# registration and runtime ownership stay simple. MSVC needs its extended
# object format once enough template-generated event dispatchers are present.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/TsWowModule.cpp"
  APPEND PROPERTY COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")

