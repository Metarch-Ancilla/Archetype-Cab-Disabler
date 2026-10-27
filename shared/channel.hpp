#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <chrono>
#include <functional>
#include <thread>
#include <atomic>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace nd {
// Bounded before UI dispatch; producers block rather than allocate unlimited posted payloads.
class ByteQueue {
    std::mutex mutex;
    std::condition_variable space, ready;
    std::deque<std::string> items;
    size_t used = 0, limit;
    bool closed = false;
  public:
    explicit ByteQueue(size_t maximum = 8 * 1024 * 1024) : limit(maximum) {}
    bool push(std::string value, bool blocking = true, bool urgent = false) {
        std::unique_lock<std::mutex> lock(mutex);
        if (value.size() > limit) return false;
        auto fits = [&] { return closed || (used <= limit - value.size() && items.size() < 1024); };
        if (urgent) { items.clear(); used = 0; }
        if (blocking && !space.wait_for(lock, std::chrono::seconds(30), fits)) return false;
        else if (!fits()) return false;
        if (closed) return false;
        used += value.size(); items.push_back(std::move(value)); ready.notify_one();
        return true;
    }
    bool pop(std::string &value, bool blocking = true) {
        std::unique_lock<std::mutex> lock(mutex);
        if (blocking) ready.wait(lock, [&] { return closed || !items.empty(); });
        if (items.empty()) return false;
        value = std::move(items.front()); items.pop_front(); used -= value.size(); space.notify_all();
        return true;
    }
    bool pending() { std::lock_guard<std::mutex> lock(mutex); return !items.empty(); }
    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true; items.clear(); used = 0; space.notify_all(); ready.notify_all();
    }
};
// The sender owns blocking OS writes; native frontends only enqueue from UI callbacks.
class CommandWriter {
    ByteQueue queue;
    std::thread worker;
    std::atomic<bool> finished{true};
    std::atomic<bool> failed{false}, closing{false};
#ifdef _WIN32
    std::atomic<HANDLE> ioThread{nullptr};
    std::atomic<int64_t> writeDeadline{0};
    std::thread watchdog;
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
#endif
  public:
    explicit CommandWriter(size_t maximum = 2 * 1024 * 1024) : queue(maximum) {}
    bool stopping() const { return closing.load(); }
    void interrupt() {
#ifdef _WIN32
        HANDLE thread=ioThread.load();if(thread)CancelSynchronousIo(thread);
#endif
    }
    template<class Send> void start(Send send, std::function<void()> failure = {}) {
        finished=false;
        worker = std::thread([this, send, failure] {
            struct Done { std::atomic<bool> &flag; ~Done(){flag=true;} } done{finished};
#ifdef _WIN32
            HANDLE thread=nullptr;
            if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&thread,0,FALSE,DUPLICATE_SAME_ACCESS)) {
                failed=true;queue.close();if(failure)failure();return;
            }
            ioThread=thread;
#endif
            std::string command;
            while (queue.pop(command)) {
#ifdef _WIN32
                writeDeadline=nowMs()+30000;
#endif
                bool sent=false;try { sent=send(command); } catch (...) {}
#ifdef _WIN32
                writeDeadline=0;
#endif
                if(sent)continue;
                failed = true; queue.close(); if(failure)failure(); break;
            }
        });
#ifdef _WIN32
        watchdog=std::thread([this] {
            while(!closing&&!finished) {
                auto deadline=writeDeadline.load();
                if(deadline&&nowMs()>=deadline)interrupt();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
#endif
    }
    bool post(std::string command, bool urgent = false) {
        return !failed && queue.push(std::move(command), false, urgent);
    }
    void close() { closing = true; queue.close(); }
    void join() {
#ifdef _WIN32
        // Repeat cancellation to cover shutdown racing the start of a synchronous write.
        while(worker.joinable()&&!finished){interrupt();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
#endif
        if (worker.joinable()) worker.join();
#ifdef _WIN32
        if(watchdog.joinable())watchdog.join();
        HANDLE thread=ioThread.exchange(nullptr);if(thread)CloseHandle(thread);
#endif
    }
    ~CommandWriter() { close(); interrupt(); join(); }
};
} // namespace nd
