cmake_minimum_required(VERSION 3.16)

project(QtScriptSmoke LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.9.2 REQUIRED COMPONENTS Core Script)

add_executable(qtscript_smoke main.cpp)
target_link_libraries(qtscript_smoke PRIVATE Qt6::Core Qt6::Script)

enable_testing()
add_test(NAME qtscript_smoke COMMAND qtscript_smoke)
