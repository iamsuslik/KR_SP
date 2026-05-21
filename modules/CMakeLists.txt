cmake_minimum_required(VERSION 3.15)
project(SimpleDBMS)

# Убираем предупреждение про скачивание
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. ПОДКЛЮЧЕНИЕ GOOGLE TEST ---
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# --- 2. ПУТИ К ХЕДЕРАМ ---
# Так как файл лежит в modules, пути указываем от неё
include_directories(
    api/include
    core/include
    logic/include
    shared/include
    storage/include
    logger/include
)

# --- 3. СБОРКА БИБЛИОТЕКИ ЛОГИКИ ---
# Собираем все .cpp файлы, кроме main.cpp и тестов
set(DB_LIB_SOURCES
    api/src/SQLParser.cpp
    core/src/HierarchyManager.cpp
    core/src/Pager.cpp
    logic/src/TableManager.cpp
    logic/src/RecordManager.cpp
    logger/src/Logger.cpp
)
add_library(db_logic STATIC ${DB_LIB_SOURCES})

# --- ОПРЕДЕЛЯЕМ ДОПОЛНИТЕЛЬНЫЕ БИБЛИОТЕКИ В ЗАВИСИМОСТИ ОТ КОМПИЛЯТОРА ---
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Для MSVC (Visual Studio на Windows) filesystem не требует дополнительных библиотек
    set(FS_LIB "")
else()
    # Для GCC/Clang на Linux/Mac нужна stdc++fs
    set(FS_LIB "stdc++fs")
endif()

# --- 4. ОСНОВНАЯ ПРОГРАММА ---
# main.cpp лежит в той же папке, что и этот CMakeLists.txt
add_executable(my_db main.cpp)
target_link_libraries(my_db db_logic ${FS_LIB})

# --- 5. GOOGLE TESTS ---
enable_testing()
# Файл тестов лежит в папке tests/ внутри modules/
add_executable(unit_tests tests/db_tests.cpp)
target_link_libraries(unit_tests db_logic gtest_main ${FS_LIB})

include(GoogleTest)
gtest_discover_tests(unit_tests)
