cmake_minimum_required(VERSION 3.15)
project(KR_SP_DBMS)

# Установка стандарта C++20 (необходим для концептов в BPlusTree.h)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. Подключение зависимостей ---
find_package(Threads REQUIRED)
set(RT_LIB "rt") # Для семафоров и Shared Memory

# Библиотека для работы с файловой системой
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    set(FS_LIB "stdc++fs")
else()
    set(FS_LIB "")
endif()

# --- 2. Настройка путей для Include (Все 6 папок) ---
include_directories(
    api/include
    core/include
    logger/include
    logic/include
    shared/include
    storage/include
)

# --- 3. Сборка основного ядра СУБД (статическая библиотека) ---
# Пути указаны относительно папки modules/
set(DB_CORE_SOURCES
    api/src/SQLParser.cpp
    api/src/AsyncManager.cpp
    core/src/AuthManager.cpp
    core/src/HierarchyManager.cpp
    core/src/Pager.cpp
    logic/src/RecordManager.cpp
    logic/src/TableManager_Queries.cpp
    logic/src/TableManager_Mutations.cpp
    shared/src/ErrorUtils.cpp
    logger/src/Logger.cpp
    logger/src/TelemetryManager.cpp
    storage/src/BPlusTree.cpp
)

add_library(db_logic STATIC ${DB_CORE_SOURCES})
target_link_libraries(db_logic PUBLIC Threads::Threads ${RT_LIB} ${FS_LIB})

# --- 4. Сборка исполняемых файлов ---
# Все файлы лежат в той же папке или подпапках modules/

# Автономное приложение
add_executable(my_db main.cpp)
target_link_libraries(my_db PRIVATE db_logic)

# Сервер
add_executable(db_server api/src/ServerApp.cpp)
target_link_libraries(db_server PRIVATE db_logic)

# Клиент
add_executable(db_client api/src/ClientApp.cpp)
target_link_libraries(db_client PRIVATE db_logic)

# --- 5. Модульные тесты (Google Test) ---
enable_testing()
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# db_tests.cpp лежит в modules/tests/
add_executable(unit_tests tests/db_tests.cpp)
target_link_libraries(unit_tests PRIVATE db_logic gtest_main)

include(GoogleTest)
gtest_discover_tests(unit_tests)

# --- 6. Настройки компилятора ---
if(NOT MSVC)
    target_compile_options(db_logic PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()