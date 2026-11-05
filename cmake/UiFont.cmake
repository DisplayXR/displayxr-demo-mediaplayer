# The UI font (Inter SemiBold, OFL-1.1, third_party/fonts/inter) as a C byte array, so no
# installer, .deb, .pkg or APK has to ship a font file. Included by both the desktop and
# the Android CMakeLists; ImGuiLayer.cpp includes ${MP_UI_FONT_INCLUDE_DIR}/ui_font.h.
set(_mp_ui_font "${CMAKE_CURRENT_LIST_DIR}/../third_party/fonts/inter/Inter-SemiBold.ttf")
set(MP_UI_FONT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ui_font")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mp_ui_font}")
file(READ "${_mp_ui_font}" _mp_ui_font_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _mp_ui_font_bytes "${_mp_ui_font_hex}")
# One line per 32 bytes: some compilers cap the length of a source line. (CMake regex has
# no {n} quantifier, hence the REPEAT.)
string(REPEAT "0x..," 32 _mp_ui_font_row)
string(REGEX REPLACE "(${_mp_ui_font_row})" "\\1\n" _mp_ui_font_bytes "${_mp_ui_font_bytes}")
# file(CONFIGURE) rewrites only on change, so a reconfigure does not rebuild ImGuiLayer.
file(CONFIGURE OUTPUT "${MP_UI_FONT_INCLUDE_DIR}/ui_font.h" @ONLY
     CONTENT "static const unsigned char kUiFontTtf[] = {\n${_mp_ui_font_bytes}};\n")
