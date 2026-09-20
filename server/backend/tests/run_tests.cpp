/**
 * @file run_tests.cpp
 * @brief Entry point for the unit tests (see CMake target s3tests).
 */

int s3ResponseTest();
int blobStoreTest();

int main()
{
    s3ResponseTest();
    blobStoreTest();
    return 0;
}
