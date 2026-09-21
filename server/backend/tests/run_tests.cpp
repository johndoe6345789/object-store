/**
 * @file run_tests.cpp
 * @brief Entry point for the unit tests (see CMake target s3tests).
 */

int s3ResponseTest();
int blobStoreTest();
int authUtilTest();
int nameUtilTest();
int multipartTest();
int sigv4Test();
int chunkedDecoderTest();
int s3MiscTest();

int main()
{
    s3ResponseTest();
    blobStoreTest();
    authUtilTest();
    nameUtilTest();
    multipartTest();
    sigv4Test();
    chunkedDecoderTest();
    s3MiscTest();
    return 0;
}
