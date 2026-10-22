cmake_minimum_required(VERSION 3.20)
# Tokyo Jungle launcher: builds the port on the player's computer from their
# own copy of the game, then starts it (launcher.cpp). Its own small project,
# so it builds before the game can: it needs only SDL2 and the stb_truetype in
# the ps3recomp submodule. ./tokyojungle.sh builds it into build-launcher/.
project(tokyojungle_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(TJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(TJ_GUIDE_URL "https://github.com/Deftones565/tokyojunglelinux/blob/main/docs/SETUP_GUIDE.md"
    CACHE STRING "The setup guide the launcher's Guide button opens")

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

add_executable(tokyojungle-launcher
    launcher.cpp
    "${TJ_ROOT}/src/game_data.cpp"
)
target_include_directories(tokyojungle-launcher PRIVATE
    "${TJ_ROOT}/src"
    "${TJ_ROOT}/third_party/ps3recomp/libs/font"   # stb_truetype.h
)
target_compile_definitions(tokyojungle-launcher PRIVATE
    TJ_SOURCE_DIR="${TJ_ROOT}"
    TJ_GUIDE_URL="${TJ_GUIDE_URL}"
)
if(TARGET SDL2::SDL2)
    target_link_libraries(tokyojungle-launcher PRIVATE SDL2::SDL2)
else()
    target_include_directories(tokyojungle-launcher PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(tokyojungle-launcher PRIVATE ${SDL2_LIBRARIES})
endif()
target_link_libraries(tokyojungle-launcher PRIVATE Threads::Threads)
target_compile_options(tokyojungle-launcher PRIVATE -Wall -Wextra -Wno-unused-parameter)
