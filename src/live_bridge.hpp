#pragma once

#include <memory>

namespace bridge {

class LiveBridge {
public:
    static LiveBridge& get();

    LiveBridge(LiveBridge const&) = delete;
    LiveBridge& operator=(LiveBridge const&) = delete;

    void start();
    void stop();
    void pumpMainThread();

private:
    LiveBridge();
    ~LiveBridge();

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bridge
