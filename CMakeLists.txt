cmake_minimum_required(VERSION 3.20)
project(maze3d)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(maze3d game.cpp)

target_link_libraries(maze3d PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    SDL2_image::SDL2_image
    nlohmann_json::nlohmann_json
)

target_include_directories(maze3d PRIVATE 
    "C:/vcpkg/installed/x64-windows/include"
)

add_custom_command(TARGET maze3d POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:maze3d>/assets"
    COMMENT "Copying game assets"
)
