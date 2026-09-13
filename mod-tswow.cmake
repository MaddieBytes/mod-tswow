set(TSWOW_SOURCE_DIR "${TSWOW_SOURCE_DIR}" CACHE PATH "Path to the TSWoW source checkout")

include(FetchContent)

if(NOT TARGET lualib OR NOT MOD_ALE_FOUND)
  message(FATAL_ERROR
    "mod-tswow requires AzerothCore mod-ale. Clone https://github.com/azerothcore/mod-ale "
    "into modules/mod-ale and configure with -DLUA_VERSION=lua54.")
endif()
if(NOT LUA_VERSION STREQUAL "lua54")
  message(FATAL_ERROR "mod-tswow requires mod-ale configured with -DLUA_VERSION=lua54")
endif()

target_link_libraries(modules PRIVATE lualib)
target_include_directories(modules PRIVATE "${MOD_ALE_PATH}/src/LuaEngine")
target_compile_definitions(modules PRIVATE TSWOW_USE_ALE)

if(NOT TARGET sol2::sol2)
  FetchContent_Declare(tswow_sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG 2b0d2fe8ba0074e16b499940c4f3126b9c7d3471)
  FetchContent_MakeAvailable(tswow_sol2)
endif()

target_link_libraries(modules PRIVATE sol2::sol2)
target_compile_definitions(modules PRIVATE SOL_NO_CHECK_NUMBER_PRECISION)

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
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/TSLuaRuntime.cpp"
  APPEND PROPERTY COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")
