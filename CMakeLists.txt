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

if(EXISTS ${WEB_DIR}/index.html)
  generate_inc_file_for_target(app
    ${WEB_DIR}/index.html ${GEN_DIR}/index.html.gz.inc --gzip)
  target_compile_definitions(app PRIVATE HAS_INDEX_HTML=1)
else()
  target_compile_definitions(app PRIVATE HAS_INDEX_HTML=0)
endif()

if(EXISTS ${WEB_DIR}/app.js)
  generate_inc_file_for_target(app
    ${WEB_DIR}/app.js ${GEN_DIR}/app.js.gz.inc --gzip)
  target_compile_definitions(app PRIVATE HAS_APP_JS=1)
else()
  target_compile_definitions(app PRIVATE HAS_APP_JS=0)
endif()

if(EXISTS ${WEB_DIR}/app.css)
  generate_inc_file_for_target(app
    ${WEB_DIR}/app.css ${GEN_DIR}/app.css.gz.inc --gzip)
  target_compile_definitions(app PRIVATE HAS_APP_CSS=1)
else()
  target_compile_definitions(app PRIVATE HAS_APP_CSS=0)
endif()
