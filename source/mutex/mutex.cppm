//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <mutex>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(__arm__)
#include <arm_acle.h>
#endif

#if defined(__linux__) && defined(__GLIBC__)
#include <pthread.h>
#endif

export module mutex;

namespace pants
{
    export inline void cpu_relax() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }

    export class spinlock final
    {
        public:

        spinlock() noexcept
        {
#if defined(__linux__) && defined(__GLIBC__)
            (void)pthread_spin_init(&lock_value, PTHREAD_PROCESS_PRIVATE);
#else
            (void)flag.test_and_set(std::memory_order_relaxed);
            flag.clear(std::memory_order_relaxed);
#endif
        }

        ~spinlock() noexcept
        {
#if defined(__linux__) && defined(__GLIBC__)
            (void)pthread_spin_destroy(&lock_value);
#endif
        }

        spinlock(const spinlock&) = delete;
        spinlock& operator=(const spinlock&) = delete;
        spinlock(spinlock&&) = delete;
        spinlock& operator=(spinlock&&) = delete;

        void lock() noexcept
        {
#if defined(__linux__) && defined(__GLIBC__)
            (void)pthread_spin_lock(&lock_value);
#else
            while (flag.test_and_set(std::memory_order_acquire))
            {
                while (flag.test(std::memory_order_relaxed))
                {
                    cpu_relax();
                }
            }
#endif
        }

        [[nodiscard]] bool try_lock() noexcept
        {
#if defined(__linux__) && defined(__GLIBC__)
            return pthread_spin_trylock(&lock_value) == 0;
#else
            return !flag.test_and_set(std::memory_order_acquire);
#endif
        }

        void unlock() noexcept
        {
#if defined(__linux__) && defined(__GLIBC__)
            (void)pthread_spin_unlock(&lock_value);
#else
            flag.clear(std::memory_order_release);
#endif
        }

        private:

#if defined(__linux__) && defined(__GLIBC__)
        pthread_spinlock_t lock_value{};
#else
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
#endif
    };

    export using spinlock_guard = std::lock_guard<spinlock>;
}
