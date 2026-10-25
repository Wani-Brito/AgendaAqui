cmake_minimum_required(VERSION 3.25)
project(AgendaAqui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR cmake-not-used
)

FetchContent_MakeAvailable(cpp_httplib nlohmann_json sqlite_amalgamation)

file(GLOB SQLITE_AMALGAMATION_DIR LIST_DIRECTORIES true
    "${sqlite_amalgamation_SOURCE_DIR}/sqlite-amalgamation-*")
if(SQLITE_AMALGAMATION_DIR)
    list(GET SQLITE_AMALGAMATION_DIR 0 SQLITE_SOURCE_DIR)
else()
    set(SQLITE_SOURCE_DIR "${sqlite_amalgamation_SOURCE_DIR}")
endif()

add_library(sqlite3 STATIC
    "${SQLITE_SOURCE_DIR}/sqlite3.c"
)
target_include_directories(sqlite3 PUBLIC "${SQLITE_SOURCE_DIR}")
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

add_executable(agendaaqui backend/main.cpp)
target_link_libraries(agendaaqui PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    sqlite3
)
target_compile_definitions(agendaaqui PRIVATE
    AGENDA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

if(WIN32)
    target_link_libraries(agendaaqui PRIVATE ws2_32 bcrypt)
endif()
