# ##############################################################################
# apps/games/infones/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

if(CONFIG_GAMES_INFONES)
  nuttx_add_application(
    NAME
    infones
    MODULE
    ${CONFIG_GAMES_INFONES}
    SRCS
    infones_main.cpp
    core/src/InfoNES.cpp
    core/src/InfoNES_Mapper.cpp
    core/src/InfoNES_pAPU.cpp
    core/src/K6502.cpp
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/src)
endif()
