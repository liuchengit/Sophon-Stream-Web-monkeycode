cmake_minimum_required(VERSION 3.15)
project(sophon-stream-web-backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find Drogon
find_package(Drogon CONFIG REQUIRED)

# Find other dependencies
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../engine/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# Source files
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.cc"
)

# Generate drogon controller registration
execute_process(
    COMMAND drogon_ctl create controller * -o ./src/controllers
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE DROGON_CTL_RESULT
)
if(NOT DROGON_CTL_RESULT EQUAL 0)
    message(WARNING "drogon_ctl create controller failed, but continuing...")
endif()

# Add executable
add_executable(sophon-stream-web ${SOURCES})

# Link libraries
target_link_libraries(sophon-stream-web
    PRIVATE
    Drogon::Drogon
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    SQLite3_lib
    sophon-stream-engine
)

# Install
install(TARGETS sophon-stream-web DESTINATION bin)

# Copy config file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.json
    ${CMAKE_CURRENT_BINARY_DIR}/config.json
    COPYONLY
)

# Create www directory for frontend build output
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/www)

# Test executables
add_executable(test_database tests/test_database.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/db/database_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/db/repositories.cpp
)
target_link_libraries(test_database PRIVATE nlohmann_json::nlohmann_json SQLite3_lib)

add_executable(test_auth tests/test_auth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/auth_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/db/database_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/db/repositories.cpp
)
target_link_libraries(test_auth PRIVATE nlohmann_json::nlohmann_json SQLite3_lib)

# Enable testing
enable_testing()
add_test(NAME DatabaseTests COMMAND test_database)
add_test(NAME AuthTests COMMAND test_auth)
