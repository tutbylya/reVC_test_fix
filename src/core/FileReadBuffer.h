#pragma once

#include <cstddef>

namespace FileReadBuffer {

// maxlen includes the trailing NUL, matching CFileMgr::LoadFile's existing
// contract. An oversized file is rejected, never returned as a partial file.
template<class Reader>
std::ptrdiff_t Read(unsigned char* buffer, int maxlen, Reader reader)
{
    if (!buffer || maxlen <= 0) return -1;
    buffer[0] = 0;
    const std::size_t capacity = static_cast<std::size_t>(maxlen - 1);
    std::size_t total = 0;
    while (total < capacity) {
        const std::size_t remaining = capacity - total;
        const std::size_t request = remaining < 0x4000 ? remaining : 0x4000;
        const std::ptrdiff_t received = reader(buffer + total, request);
        if (received < 0 || static_cast<std::size_t>(received) > request) {
            buffer[0] = 0;
            return -1;
        }
        total += static_cast<std::size_t>(received);
        buffer[total] = 0;
        if (static_cast<std::size_t>(received) < request) return static_cast<std::ptrdiff_t>(total);
    }
    // Exact fit is valid only if the file really ends here. Keep the overflow
    // probe on the stack, not one byte beyond the caller's buffer.
    unsigned char extra;
    if (reader(&extra, 1) != 0) { buffer[0] = 0; return -1; }
    return static_cast<std::ptrdiff_t>(total);
}

} // namespace FileReadBuffer
