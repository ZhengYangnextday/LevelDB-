#include <iostream>
#include <atomic>
#include <thread>

std::atomic<int> counter(0);

void increment() {
    for (int i = 0; i < 100; ++i) {
        counter.fetch_add(1);
    }
}

/**
 * @brief From ChatGPT:
 * 在上面的代码中，我们创建了两个线程，它们共享一个计数器。
 * 每个线程通过fetch_add(1)原子地将计数器增加100次。当两个线程都完成时，我们输出计数器的最终值。
 * 由于fetch_add()是原子操作，因此我们可以保证多个线程之间对计数器的访问是安全的，最终输出的结果也应该是正确的。
    除了fetch_add()，std::atomic<T>类还提供了其他的原子操作函数，
    例如fetch_sub()、fetch_and()、fetch_or()等等，以便我们在多线程环境中实现原子操作。

    因为两个线程共同执行了100次fetch_add(1)操作，所以最终计数器的值应该是200。
    由于这个操作是原子的，多个线程对同一个计数器的操作不会相互干扰，因此输出的结果应该是正确的。
 * 
 * @return int 
 */
int main() {
    std::thread t1(increment);
    std::thread t2(increment);

    t1.join();
    t2.join();

    std::cout << "Counter value: " << counter << std::endl;

    return 0;
}