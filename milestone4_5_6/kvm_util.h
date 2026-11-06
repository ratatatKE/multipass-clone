// kvm_util.h = small helpers carried over from milestone 1
#pragma once
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace tinyvm
{

    // Turn "-1 + errno" that we get in kernel development into an exception so the happy path stays readable
    inline int check(int rc, const char *what)
    {
        if (rc < 0)
            throw std::system_error(errno, std::generic_category(), what);
        return rc;
    }

    // RAII file descriptor: KVM gives us fds for the system, the VM and each vCPU. That's how KVM works
    class Fd
    {
    public:
        Fd() = default;
        explicit Fd(int fd) : fd_(fd) {}
        Fd(Fd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
        Fd &operator=(Fd &&o) noexcept
        {
            if (this != &o)
            { // if not same object
                reset();
                fd_ = std::exchange(o.fd_, -1);
            }
            return *this;
        }

        Fd(const Fd &) = delete; // can't be copied.
        Fd &operator=(const Fd &) = delete;

        ~Fd() { reset(); }
        int get() const { return fd_; }

    private:
        void reset()
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = -1;
        }
        int fd_ = -1;
    };

    /*
        Guest RAM is anonymous HOST Memory. The VMM writes to it directly through a HOST pointer;
        The guest sees the same bytes at the guest-physical address
    */
    class GuestMemory
    {
    public:
        explicit GuestMemory(size_t size) : size_(size)
        {
            void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p == MAP_FAILED)
                throw std::system_error(errno, std::generic_category(), "mmap guest RAM");
            base_ = static_cast<uint8_t *>(p);
        }

        ~GuestMemory() { munmap(base_, size_); }
        GuestMemory(const GuestMemory &) = delete;
        GuestMemory &operator=(const GuestMemory &) = delete;

        uint8_t *host_ptr(uint64_t gpa) const { return base_ + gpa; } // GPA -> HVA
        size_t size() const { return size_; }

        void write(uint64_t gpa, const void *src, size_t len)
        {
            if (gpa + len > size_)
                throw std::out_of_range("write outside guest RAM");
            std::memcpy(host_ptr(gpa), src, len);
        }

    private:
        uint8_t *base_ = nullptr;
        size_t size_;
    };

} // namespace tinyvm