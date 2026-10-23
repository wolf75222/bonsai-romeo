cmake_minimum_required(VERSION 3.14)
project(bonsai-romeo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui.git
  GIT_TAG v7.0.3
)
FetchContent_MakeAvailable(ftxui)

set(SRC
    src/main.cpp

    src/core/app_data.cpp
    src/core/dir_tree.cpp
    src/core/scanner.cpp
    src/core/quota.cpp
    src/core/walker.cpp

    src/ui/cleanup_view.cpp
    src/ui/menu.cpp
    src/ui/modal_confirm.cpp
    src/ui/modal_error.cpp
    src/ui/piechart.cpp
    src/ui/search_view.cpp
    src/ui/quota_view.cpp
)

# Embed default config
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(GENERATED_HEADER_NAME defaults)
set(DEFAULT_CONF ${CMAKE_CURRENT_SOURCE_DIR}/res/bonsai.default.conf)
set(GENERATED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/gen/${GENERATED_HEADER_NAME}.hpp)

# Done by CMake itself: the xxd on ROMEO is too old for "xxd -n"
file(READ ${DEFAULT_CONF} DEFAULT_CONF_HEX HEX)
string(LENGTH "${DEFAULT_CONF_HEX}" DEFAULT_CONF_HEX_LEN)
math(EXPR DEFAULT_CONF_LEN "${DEFAULT_CONF_HEX_LEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," DEFAULT_CONF_BYTES "${DEFAULT_CONF_HEX}")
file(WRITE ${GENERATED_HEADER}
    "inline unsigned char bonsai_default[] = {${DEFAULT_CONF_BYTES}};\n"
    "inline unsigned int bonsai_default_len = ${DEFAULT_CONF_LEN};\n"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DEFAULT_CONF})

include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_executable(bonsai-romeo ${SRC})

target_compile_options(bonsai-romeo PRIVATE
    $<$<CONFIG:Release>:-O3>
)

target_link_libraries(bonsai-romeo
    ftxui::screen
    ftxui::dom
    ftxui::component
)
