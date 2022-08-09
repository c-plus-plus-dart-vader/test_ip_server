cmake_minimum_required(VERSION 3.16)

project(protey_server)

set(CMAKE_CXX_STANDARD 17)

include_directories(
${CMAKE_CURRENT_SOURCE_DIR}/../common
)

add_executable(${PROJECT_NAME} main.cpp Server.cpp)