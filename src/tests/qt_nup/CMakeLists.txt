# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(nup_dialog_tests
    nup_dialog.cpp
    ../../lichii/nup_dialogs.cpp
    ../../lichii/nup_dialogs.h
    ../../lichii/util/icon_search.cpp
    ../../lichii/util/icon_search.h
)
target_include_directories(nup_dialog_tests PRIVATE ../..)
target_link_libraries(nup_dialog_tests PRIVATE
    common core qt_common frontend_common video_core Qt6::Widgets Qt6::Concurrent Qt6::Test
    ${PLATFORM_LIBRARIES}
)
if(NOT MSVC)
    target_compile_options(nup_dialog_tests PRIVATE
        -Wno-unused-variable -Wno-missing-field-initializers)
endif()
add_test(NAME nup_dialog COMMAND nup_dialog_tests)
set_tests_properties(nup_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
