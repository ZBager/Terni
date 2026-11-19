# SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
# Copyright (C) 2026 Rafał Zając
# cmake -DTOOL=<program> -DFOLDER_FILE=<file> -DARGS=<a|b|...> -DEXPECT=<regex> -P run_in_utf8_folder.cmake
# Runs a tool with arguments in which @FOLDER@ stands for the folder FOLDER_FILE names
# (test_paths writes it). The name is not ASCII, and no source file may hold it.
file(READ "${FOLDER_FILE}" folder)
string(REPLACE "@FOLDER@" "${folder}" args "${ARGS}")
string(REPLACE "|" ";" args "${args}")
execute_process(COMMAND "${TOOL}" ${args} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "exit code ${code}")
endif()
if(NOT "${out}${err}" MATCHES "${EXPECT}")
  message(FATAL_ERROR "the output does not match '${EXPECT}'")
endif()
