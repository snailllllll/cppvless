#ifndef VMESS_NET_IO_URING_CPP
#define VMESS_NET_IO_URING_CPP

#include "net/io_uring.h"
#include "net/socket.h"
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace vmess {
namespace net {

// ============ IoUring 实现 ============

IoUring::IoUring(unsigned int entries) 
    : ring_(nullptr), buffers_(nullptr), bufferSize_(0), bufferCount_(0), groupId_(1337) {
    
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
    if (buffers_) {
        delete[] buffers_;
        buffers_ = nullptr;
    }
}

IoUring::IoUring(IoUring&& other) noexcept
    : ring_(other.ring_), buffers_(other.buffers_),
      bufferSize_(other.bufferSize_), bufferCount_(other.bufferCount_),
      groupId_(other.groupId_) {
    other.ring_ = nullptr;
    other.buffers_ = nullptr;
}

IoUring& IoUring::operator=(IoUring&& other) noexcept {
    if (this != &other) {
        if (ring_) {
            io_uring_queue_exit(ring_);
            delete ring_;
        }
        if (buffers_) {
            delete[] buffers_;
        }
        ring_ = other.ring_;
        buffers_ = other.buffers_;
        bufferSize_ = other.bufferSize_;
        bufferCount_ = other.bufferCount_;
        groupId_ = other.groupId_;
        other.ring_ = nullptr;
        other.buffers_ = nullptr;
    }
    return *this;
}

bool IoUring::checkFeatures() const {
    if (!ring_) return false;
    
    struct io_uring_probe* probe = io_uring_get_probe_ring(ring_);
    if (!probe) return false;
    
    bool supported = io_uring_opcode_supported(probe, IORING_OP_PROVIDE_BUFFERS);
    io_uring_free_probe(probe);
    
    return supported;
}

bool IoUring::initBuffers(unsigned groupId, size_t bufferSize, size_t bufferCount) {
    if (!ring_ || buffers_) return false;
    
    groupId_ = groupId;
    bufferSize_ = bufferSize;
    bufferCount_ = bufferCount;
    
    // 分配对齐的内存
    buffers_ = new char[bufferCount * bufferSize];
    
    // 提交 provide_buffers SQE
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    io_uring_prep_provide_buffers(sqe, buffers_, bufferSize, bufferCount, groupId, 0);
    
    // 同步等待完成
    io_uring_submit(ring_);
    
    struct io_uring_cqe* cqe;
    io_uring_wait_cqe(ring_, &cqe);
    
    bool success = (cqe->res >= 0);
    io_uring_cqe_seen(ring_, cqe);
    
    if (!success) {
        delete[] buffers_;
        buffers_ = nullptr;
    }
    
    return success;
}

void IoUring::provideBuffer(unsigned groupId, uint16_t bid) {
    if (!ring_ || !buffers_) return;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    io_uring_prep_provide_buffers(sqe, buffers_ + bid * bufferSize_, 
                                   bufferSize_, 1, groupId, bid);
    
    UringRequest req;
    req.fd = 0;
    req.type = (uint16_t)UringEventType::PROV_BUF;
    req.bid = bid;
    sqe->user_data = req.toUserData();
}

bool IoUring::prepareAccept(int listenFd, struct sockaddr* clientAddr, 
                               socklen_t* addrLen, unsigned flags) {
    if (!ring_) return false;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return false;
    
    io_uring_prep_accept(sqe, listenFd, clientAddr, addrLen, 0);
    io_uring_sqe_set_flags(sqe, flags);
    
    setUserData(sqe, listenFd, UringEventType::ACCEPT);
    
    return true;
}

bool IoUring::prepareRecv(int fd, unsigned groupId, size_t bufferSize, unsigned flags) {
    if (!ring_) return false;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return false;
    
    io_uring_prep_recv(sqe, fd, nullptr, bufferSize, 0);
    io_uring_sqe_set_flags(sqe, flags);
    sqe->buf_group = groupId;
    
    setUserData(sqe, fd, UringEventType::READ);
    
    return true;
}

bool IoUring::prepareRecv(int fd, void* buf, size_t len, unsigned flags) {
    if (!ring_) return false;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return false;
    
    io_uring_prep_recv(sqe, fd, buf, len, 0);
    io_uring_sqe_set_flags(sqe, flags);
    
    setUserData(sqe, fd, UringEventType::READ);
    
    return true;
}

bool IoUring::prepareSend(int fd, const void* buf, size_t len, unsigned flags) {
    if (!ring_) return false;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return false;
    
    io_uring_prep_send(sqe, fd, buf, len, 0);
    io_uring_sqe_set_flags(sqe, flags);
    
    setUserData(sqe, fd, UringEventType::WRITE);
    
    return true;
}

bool IoUring::prepareShutdown(int fd) {
    if (!ring_) return false;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return false;
    
    io_uring_prep_shutdown(sqe, fd, SHUT_RDWR);
    
    setUserData(sqe, fd, UringEventType::WRITE);
    
    return true;
}

bool IoUring::prepareClose(int fd) {
    if (!ring_) return false;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) return false;
    
    io_uring_prep_close(sqe, fd);
    
    setUserData(sqe, fd, UringEventType::WRITE);
    
    return true;
}

int IoUring::submitAll() {
    if (!ring_) return 0;
    return io_uring_submit(ring_);
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
        
        UringRequest req = UringRequest::fromUserData(cqe->user_data);
        callback(req, cqe->res, cqe->flags);
    }
    
    io_uring_cq_advance(ring_, count);
}

bool IoUring::runOnce(const UringCallback& callback) {
    if (!ring_) return false;
    
    submitAndWait(1);
    
    struct io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;
    
    io_uring_for_each_cqe(ring_, head, cqe) {
        ++count;
        
        UringRequest req = UringRequest::fromUserData(cqe->user_data);
        callback(req, cqe->res, cqe->flags);
    }
    
    io_uring_cq_advance(ring_, count);
    
    return count > 0;
}

char* IoUring::getBuffer(uint16_t bid) {
    if (!buffers_ || bid >= bufferCount_) return nullptr;
    return buffers_ + bid * bufferSize_;
}

uint16_t IoUring::getBufferId(const char* buffer) const {
    if (!buffers_ || !buffer) return 0;
    return (uint16_t)((buffer - buffers_) / bufferSize_);
}

void IoUring::setUserData(struct io_uring_sqe* sqe, int fd, 
                              UringEventType type, uint16_t bid) {
    UringRequest req;
    req.fd = fd;
    req.type = (uint16_t)type;
    req.bid = bid;
    sqe->user_data = req.toUserData();
}

}  // namespace net
}  // namespace vmess

#endif  // VMESS_NET_IO_URING_CPP
