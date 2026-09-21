cmake_minimum_required(VERSION 3.20)
project(s3server-backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Drogon REQUIRED)
find_package(OpenSSL REQUIRED)

# SigV4: canonicalisation, verification, aws-chunked decoding, payload checks.
set(S3_SIGV4_SOURCES
    src/sigv4/Canonical.cpp
    src/sigv4/SigV4Verify.cpp
    src/sigv4/PresignedAuth.cpp
    src/sigv4/ChunkedDecoder.cpp
    src/sigv4/PayloadReader.cpp
)

add_executable(s3server
    src/main.cpp
    src/controllers/S3Router.cpp
    src/controllers/BucketOps.cpp
    src/controllers/ListOps.cpp
    src/controllers/ObjectGet.cpp
    src/controllers/ObjectPut.cpp
    src/controllers/MultipartOps.cpp
    src/controllers/MultipartList.cpp
    src/filters/AuthFilter.cpp
    src/services/BucketStore.cpp
    src/services/ObjectStoreList.cpp
    src/services/KeyStore.cpp
    src/services/SecretCrypto.cpp
    ${S3_SIGV4_SOURCES}
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
    tests/auth_util_test.cpp
    tests/name_util_test.cpp
    tests/multipart_test.cpp
    tests/sigv4_test.cpp
    tests/chunked_decoder_test.cpp
    tests/s3_misc_test.cpp
    src/services/SecretCrypto.cpp
    ${S3_SIGV4_SOURCES}
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

# The tests are plain assert()s, and Release defines NDEBUG, which turns every
# assert into nothing -- the tests would "pass" while checking nothing.
target_compile_options(s3tests PRIVATE -UNDEBUG)

add_test(NAME s3tests COMMAND s3tests)
