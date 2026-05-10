# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(auto-damper)

FILE(GLOB_RECURSE app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE include)

# Linker section for HTTP resources
zephyr_linker_sources(SECTIONS sections-rom.ld)

# Web resources (gzip-compressed, embedded in firmware)
set(WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated/)

# index.html — always revalidated (references hashed assets)
if(EXISTS ${WEB_DIR}/index.html)
  generate_inc_file_for_target(app
    ${WEB_DIR}/index.html ${GEN_DIR}/index.html.gz.inc --gzip)
  target_compile_definitions(app PRIVATE HAS_INDEX_HTML=1)
else()
  target_compile_definitions(app PRIVATE HAS_INDEX_HTML=0)
endif()

# Hashed assets — glob web/assets/ and generate inc files + manifest
file(GLOB WEB_ASSETS ${WEB_DIR}/assets/*)

set(ASSET_MANIFEST "")
set(ASSET_INDEX 0)

foreach(ASSET_FILE ${WEB_ASSETS})
  get_filename_component(ASSET_NAME ${ASSET_FILE} NAME)
  string(MAKE_C_IDENTIFIER ${ASSET_NAME} ASSET_ID)

  generate_inc_file_for_target(app
    ${ASSET_FILE} ${GEN_DIR}/asset_${ASSET_ID}.gz.inc --gzip)

  string(APPEND ASSET_MANIFEST
    "static const uint8_t asset_${ASSET_ID}_data[] = {\n"
    "#include \"asset_${ASSET_ID}.gz.inc\"\n"
    "};\n\n")

  # Determine content type from extension
  get_filename_component(ASSET_EXT ${ASSET_FILE} LAST_EXT)
  if(ASSET_EXT STREQUAL ".js")
    set(CONTENT_TYPE "application/javascript")
  elseif(ASSET_EXT STREQUAL ".css")
    set(CONTENT_TYPE "text/css")
  elseif(ASSET_EXT STREQUAL ".svg")
    set(CONTENT_TYPE "image/svg+xml")
  elseif(ASSET_EXT STREQUAL ".ico")
    set(CONTENT_TYPE "image/x-icon")
  elseif(ASSET_EXT STREQUAL ".woff2")
    set(CONTENT_TYPE "font/woff2")
  else()
    set(CONTENT_TYPE "application/octet-stream")
  endif()

  string(APPEND ASSET_ENTRIES
    "  { \"/assets/${ASSET_NAME}\", asset_${ASSET_ID}_data, sizeof(asset_${ASSET_ID}_data), \"${CONTENT_TYPE}\" },\n")

  math(EXPR ASSET_INDEX "${ASSET_INDEX} + 1")
endforeach()

# Generate the asset manifest header
file(WRITE ${GEN_DIR}/web_assets.h
  "/* Auto-generated — do not edit */\n"
  "#ifndef WEB_ASSETS_H\n"
  "#define WEB_ASSETS_H\n\n"
  "#include <stdint.h>\n"
  "#include <stddef.h>\n\n"
  "struct web_asset {\n"
  "  const char *path;\n"
  "  const uint8_t *data;\n"
  "  size_t len;\n"
  "  const char *content_type;\n"
  "};\n\n"
  "${ASSET_MANIFEST}"
  "static const struct web_asset web_assets[] = {\n"
  "${ASSET_ENTRIES}"
  "};\n\n"
  "#endif\n")

target_compile_definitions(app PRIVATE WEB_ASSET_COUNT=${ASSET_INDEX})
