/* One value worked out on a worker thread, so slow startup work (the GPU probe, the texture pack
 * scans) never stalls the window: start() returns at once, ready() never blocks and joins the
 * finished worker, get() is the value once ready. The work gets a cancel flag that cancel() sets
 * before it joins (the launcher exiting). The job lives where it was made (the worker points at
 * it), so it is neither copied nor moved; the destructor cancels and joins a running worker. When
 * no thread can be started, start() does the work at once on the caller's thread. */
#ifndef LAUNCHER_ASYNC_H
#define LAUNCHER_ASYNC_H
#include "platform.h"
#include <atomic>
#include <functional>

namespace launcher {

template <class T> class Async {
public:
    using Work = std::function<T(const std::atomic<bool> &cancel)>;

    Async() = default;
    Async(const Async &) = delete;
    Async &operator=(const Async &) = delete;
    ~Async() { cancel(); }

    /* False when it was started before. */
    bool start(Work work) {
        if (started_) return false;
        started_ = true;
        work_ = std::move(work);
        if (!thread_start(thread_, run, this)) {
            value_ = work_(cancel_);
            joined_ = true;
            done_ = true;
        }
        return true;
    }
    bool started() const { return started_; }
    /* True once the value is in; joins the worker the first time it sees it done. */
    bool ready() {
        if (!started_ || !done_) return false;
        if (!joined_) thread_join(thread_), joined_ = true;
        return true;
    }
    /* Blocks until the value is in. */
    void wait() {
        if (!started_ || joined_) return;
        thread_join(thread_);
        joined_ = true;
    }
    /* Asks the work to stop and waits for it. */
    void cancel() {
        cancel_ = true;
        wait();
    }
    const T &get() const { return value_; }

private:
    static void run(void *arg) {
        Async *a = (Async *)arg;
        a->value_ = a->work_(a->cancel_);
        a->done_ = true;
    }

    Work work_;
    T value_{};
    Thread thread_;
    std::atomic<bool> done_{false}, cancel_{false};
    bool started_ = false, joined_ = false;
};

} // namespace launcher
#endif
