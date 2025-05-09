# Copyright (c) 2017 Cisco and/or its affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

set(HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(COMMON_SOURCE_FILES

)

list(APPEND EXAMPLES_LIST

)

foreach (EXAMPLE_SRC ${EXAMPLES_LIST})
  string(FIND ${EXAMPLE_SRC} "/" INDEX)
  string(SUBSTRING ${EXAMPLE_SRC} 0 ${INDEX} EXECUTABLE)
  add_executable(${EXECUTABLE} ${COMMON_SOURCE_FILES} ${EXAMPLE_SRC})
  target_include_directories(${EXECUTABLE} PRIVATE $<BUILD_INTERFACE:${HEADERS_DIR}>)
  target_link_libraries(${EXECUTABLE} memif ${CMAKE_THREAD_LIBS_INIT})
endforeach()
