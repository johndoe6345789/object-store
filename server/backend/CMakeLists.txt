cmake_minimum_required(VERSION 3.20)
project(s3server-backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Drogon REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(s3server
    src/main.cpp
    src/controllers/BucketCtrl.cpp
    src/controllers/ObjectCtrl.cpp
    src/controllers/ObjectMutCtrl.cpp
    src/controllers/ListCtrl.cpp
    src/controllers/HealthCtrl.cpp
    src/filters/AuthFilter.cpp
    src/services/BucketStore.cpp
    src/services/ObjectStoreList.cpp
)

target_include_directories(s3server
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(s3server
    PRIVATE
        Drogon::Drogon
        OpenSSL::SSL
        OpenSSL::Crypto
)

install(TARGETS s3server
    RUNTIME DESTINATION bin
)

# Unit tests: plain executables with asserts, no framework to install.
# Nothing built them before, so they could not fail.
#   cmake --build out --target s3tests && ./out/s3tests   (or ctest)
enable_testing()

add_executable(s3tests EXCLUDE_FROM_ALL
    tests/run_tests.cpp
    tests/s3_response_test.cpp
    tests/blob_store_test.cpp
)

target_include_directories(s3tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(s3tests
    PRIVATE
        Drogon::Drogon
        OpenSSL::SSL
        OpenSSL::Crypto
)

add_test(NAME s3tests COMMAND s3tests)
