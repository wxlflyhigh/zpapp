# Copyright (c) 2024 Realtek Semiconductor Corp.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -save-temps=obj")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fatfs)

target_sources(app PRIVATE src/main.c)
