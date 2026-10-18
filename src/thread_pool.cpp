#include "thread_pool.hpp"

std::size_t ThreadPool::defaultSize()
{
    const unsigned hint = std::thread::hardware_concurrency();
    return hint == 0 ? 2 : hint;
}

ThreadPool::ThreadPool(std::size_t threads)
{
    _threads.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        _threads.emplace_back(&ThreadPool::work, this);  // el pool vive hasta después del join (destructor)
    }
}

ThreadPool::~ThreadPool()
{
    std::deque<std::function<void()>> discarded;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _closed = true;
        discarded.swap(_jobs);  // se destruyen fuera del lock: sus promises quedan sin valor (broken_promise)
    }
    _ready.notify_all();
    for (std::thread& t : _threads) {
        t.join();
    }
}

void ThreadPool::work()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            // Predicado: despertares espurios, y el cierre (despierta a todos para terminar).
            _ready.wait(lock, [this] { return _closed || !_jobs.empty(); });
            if (_closed) {
                return;
            }
            job = std::move(_jobs.front());
            _jobs.pop_front();
        }
        job();  // fuera del lock: los demás hilos siguen sacando trabajos mientras este calcula
    }
}
