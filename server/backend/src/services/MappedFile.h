/**
 * @file MappedFile.h
 * @brief Read-only mmap of a file, so big bodies are addressed as a
 *        string_view without being resident or copied.
 */

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace s3
{

class MappedFile
{
  public:
    /// @param unlinkOnClose remove the file when this object goes away
    ///        (decoded request bodies); false for blobs that stay.
    MappedFile(const std::filesystem::path& p, bool unlinkOnClose)
        : path_(p), unlink_(unlinkOnClose)
    {
        int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw std::runtime_error("open failed");
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("fstat failed");
        }
        size_ = static_cast<size_t>(st.st_size);
        if (size_ > 0) {
            addr_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (addr_ == MAP_FAILED) {
                ::close(fd);
                addr_ = nullptr;
                throw std::runtime_error("mmap failed");
            }
        }
        ::close(fd);
    }
    ~MappedFile()
    {
        if (addr_)
            ::munmap(addr_, size_);
        if (unlink_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::string_view view() const
    {
        return {static_cast<const char*>(addr_), size_};
    }

  private:
    std::filesystem::path path_;
    bool unlink_;
    void* addr_ = nullptr;
    size_t size_ = 0;
};

} // namespace s3
