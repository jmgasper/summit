#pragma once
#include <cstdint>
#include <cstdio>
#include <sys/types.h>
typedef int32_t status_t;
typedef uint32_t uint32;
typedef uint8_t uint8;
enum { B_OK = 0, B_BAD_VALUE = -2147483643, B_INTERRUPTED = -2147483638, B_IO_ERROR = -2147483647 + 1, B_NOT_ALLOWED = -2147483633 };
class BPositionIO {
public:
    virtual ~BPositionIO() = default;
    virtual ssize_t ReadAt(off_t, void*, size_t) = 0;
    virtual ssize_t WriteAt(off_t, const void*, size_t) = 0;
    virtual off_t Seek(off_t, uint32) = 0;
    virtual off_t Position() const = 0;
    virtual status_t GetSize(off_t*) const = 0;
    virtual status_t SetSize(off_t) = 0;
};
