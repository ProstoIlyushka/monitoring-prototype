cmake_minimum_required(VERSION 3.10)
project(monitoring-server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)

add_executable(monitoring-server main.cpp)
target_link_libraries(monitoring-server Drogon::Drogon)
