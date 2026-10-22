#pragma once
#include "shm_layout.hpp"

#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <opencv2/imgcodecs.hpp>

namespace bawal::ipc {

class ShmFrameConsumer {
public:
    ShmFrameConsumer() = default;
    ~ShmFrameConsumer() { cleanup(); }
    bool init();
    bool waitForNextFrame(cv::Mat& out_bgr_frame, uint32_t& out_frame_id);

private:
    void cleanup();
    int m_shm_fd{-1};
    int m_server_sock{-1};
    int m_client_sock{-1};
    uint8_t* m_base_ptr{nullptr};
    ShmGlobalHeader* m_global_hdr{nullptr};
    uint64_t m_last_seq{0};
};
};