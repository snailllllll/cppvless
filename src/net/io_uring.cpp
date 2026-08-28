#ifndef VLESS_NET_IO_URING_CPP
#define VLESS_NET_IO_URING_CPP

#include "net/io_uring.h"
#include <cstring>
#include <stdexcept>

namespace vless {
namespace net {

// ============ IoUring 实现 ============

IoUring::IoUring(unsigned int entries)
    : ring_(nullptr) {

    ring_ = new struct io_uring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    if (io_uring_queue_init_params(entries, ring_, &params) < 0) {
        delete ring_;
        ring_ = nullptr;
        throw std::runtime_error("io_uring_queue_init_params failed");
    }
}

IoUring::~IoUring() {
    if (ring_) {
        io_uring_queue_exit(ring_);
        delete ring_;
        ring_ = nullptr;
    }
}

IoUring::IoUring(IoUring&& other) noexcept
    : ring_(other.ring_) {
    other.ring_ = nullptr;
}

IoUring& IoUring::operator=(IoUring&& other) noexcept {
    if (this != &other) {
        if (ring_) {
            io_uring_queue_exit(ring_);
            delete ring_;
        }
        ring_ = other.ring_;
        other.ring_ = nullptr;
    }
    return *this;
}

int IoUring::submitAndWait(unsigned waitNum) {
    if (!ring_) return 0;
    return io_uring_submit_and_wait(ring_, waitNum);
}

void IoUring::processCompletions(const UringCallback& callback) {
    if (!ring_) return;

    struct io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(ring_, head, cqe) {
        ++count;
        callback(cqe->res, cqe->flags, cqe->user_data);
    }

    io_uring_cq_advance(ring_, count);
}

}  // namespace net
}  // namespace vless

#endif  // VLESS_NET_IO_URING_CPP
