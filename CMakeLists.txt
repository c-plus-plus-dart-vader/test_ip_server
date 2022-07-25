cmake_minimum_required(VERSION 3.16)

project(protey_server)

set(CMAKE_CXX_STANDARD 17)

add_executable(${PROJECT_NAME} main.cpp Server.cpp)

target_link_libraries(${PROJECT_NAME})