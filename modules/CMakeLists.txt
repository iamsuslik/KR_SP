cmake_minimum_required(VERSION 3.15)
project(SimpleDBMS)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. GOOGLE TEST ---
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# --- 2. ПУТИ ---
include_directories(
    api/include
    core/include
    logic/include
    shared/include
    storage/include
    logger/include
)

# --- 3. СБОРКА БИБЛИОТЕКИ ---
set(DB_LIB_SOURCES
    api/src/SQLParser.cpp
    api/src/AsyncManager.cpp
    core/src/HierarchyManager.cpp
    core/src/Pager.cpp
    logic/src/TableManager_Queries.cpp
    logic/src/TableManager_Mutations.cpp
    logic/src/RecordManager.cpp
    logger/src/Logger.cpp
    shared/src/ErrorUtils.cpp
    logger/src/TelemetryManager.cpp
)
add_library(db_logic STATIC ${DB_LIB_SOURCES})

# Потоки обязательны для AsyncManager
find_package(Threads REQUIRED)
target_link_libraries(db_logic PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(FS_LIB "")
    target_compile_options(db_logic PRIVATE /W4)
else()
    set(FS_LIB "stdc++fs")
    target_compile_options(db_logic PRIVATE -Wall -Wextra)
endif()

# --- 4. EXECUTABLES ---
add_executable(my_db main.cpp)
target_link_libraries(my_db db_logic ${FS_LIB})

# --- 5. TESTS ---
enable_testing()
add_executable(unit_tests tests/db_tests.cpp)
target_link_libraries(unit_tests db_logic gtest_main ${FS_LIB})

include(GoogleTest)
gtest_discover_tests(unit_tests)

# --- СЕРВЕР ---
add_executable(db_server api/src/ServerApp.cpp)
target_link_libraries(db_server db_logic ${FS_LIB})

# --- КЛИЕНТ ---
add_executable(db_client api/src/ClientApp.cpp)
target_link_libraries(db_client db_logic ${FS_LIB})
