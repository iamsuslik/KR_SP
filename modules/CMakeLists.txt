cmake_minimum_required(VERSION 3.15)
project(SimpleDBMS)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ..    
    api/include
    core/include
    logic/include
    shared/include
    storage/include
)

set(SOURCES
    main.cpp
    api/src/SQLParser.cpp
    core/src/HierarchyManager.cpp
    core/src/Pager.cpp
    logic/src/TableManager.cpp
    storage/src/BPlusTree.cpp
    logger/src/Logger.cpp
    logic/src/RecordManager.cpp
)

add_executable(my_db ${SOURCES})

if(UNIX)
    target_link_libraries(my_db stdc++fs)
endif()
