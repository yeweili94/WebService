#include <WebServer/Buffer.h>
#include <stdio.h>

using namespace ywl;
using namespace ywl::net;

const char Buffer::kCRLF[] = "\r\n";

const size_t Buffer::kCheapPrependSize = 8;
const size_t Buffer::kInitialSize  = 1024;

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    // saved an ioctl()/FIONREAD call to tell how much to read
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t writable = writeableBytes();
    vec[0].iov_base = begin() + write_index_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;
    // when there is enough space in this buffer, don't read into extrabuf.
    // when extrabuf is used, we read 64k bytes at most.
    const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        write_index_ += n;
    } else {
        write_index_ = capacity_;
        append(extrabuf, n - writable);
    }

    return n;
}
