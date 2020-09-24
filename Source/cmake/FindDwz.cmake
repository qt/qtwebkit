# - Find dwz
# This module looks for dwz. This module defines the
# following values:
#
#  DWZ_EXECUTABLE          - The full path to the dwz tool.
#  DWZ_FOUND               - True if dwz has been found.
#  DWZ_VERSION_STRING      - dwz version found, e.g. 0.12
#
# Copyright (C) 2019 Konstantin Tokarev <annulen@yandex.ru>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1.  Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
# 2.  Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND ITS CONTRIBUTORS ``AS
# IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR ITS
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

find_program(DWZ_EXECUTABLE NAMES dwz)

if (DWZ_EXECUTABLE)
    execute_process(
        COMMAND "${DWZ_EXECUTABLE}" -v
        RESULT_VARIABLE _DWZ_VERSION_RESULT
        ERROR_VARIABLE _DWZ_VERSION_OUTPUT
        OUTPUT_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (NOT _DWZ_VERSION_RESULT AND _DWZ_VERSION_OUTPUT MATCHES "dwz version ([0-9\\.]+)")
        set(DWZ_VERSION_STRING "${CMAKE_MATCH_1}")
    endif ()
endif ()

# handle the QUIETLY and REQUIRED arguments and set DWZ_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Dwz
                                  REQUIRED_VARS DWZ_EXECUTABLE
                                  VERSION_VAR DWZ_VERSION_STRING)

mark_as_advanced(DWZ_EXECUTABLE)
