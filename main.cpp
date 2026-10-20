#include <iostream>
#include <array>
#include <atomic>
#include <optional>
#include <thread>

template <typename T, std::size_t N>
class SPSCQ{
    private:
    std::array<T, N> buf_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    public:
    bool push(const T& item){
        //push is the writer, it writes to tail, but only READS head, which is written by pop.
        const auto t = tail_.load(std::memory_order_relaxed);
        const auto next = (t + 1) % N; 
        //now it needs to check if its not FULL.
        if( next == head_.load(std::memory_order_acquire) ){
            //std::cout<<"queue is full"<<std::endl;
            return false;
        }
        buf_[t] = item;
        //release order for tail, as this is required for pop
        tail_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop(){
        //this is aka reverse of wat push() was doing, here we are manipulating the head_.
        const auto h = head_.load(std::memory_order_relaxed);
        //now check if its EMPTY
        if(h == tail_.load(std::memory_order_acquire) ){
            //std::cout<<"queue is empty"<<std::endl;
            return std::nullopt;
        }
        auto item = buf_[h];
        head_.store( (h+1)%N, std::memory_order_release);
        return item;
    }
};


int main(void){
    std::cout<<"into SPSC"<<std::endl;
    //Now the main runner with one queue and one producer and one consumer thread.
    SPSCQ<int, 1024> q;
    constexpr int kCount = 1000000;
    int errors = 0;

    {
        std::jthread producer([&q]{
                for(int i = 0; i < kCount; ++i){
                    while(!q.push(i)) std::this_thread::yield();
                }
            });

            std::jthread consumer([&q, &errors]{
                int expected = 0;
                while(expected < kCount){
                    if(auto v = q.pop()){
                        if(*v != expected) ++errors;
                        ++expected;
                    }
                    else{
                        std::this_thread::yield();
                    }
                }
            });

    }
 
    if (errors == 0) std::cout << "OK: " << kCount << " items arrived in order\n";
    else             std::cout << "FAILED: " << errors << " items wrong\n";

    return 0;
}