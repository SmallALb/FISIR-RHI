#pragma once
#include <optional>
#include <semaphore>
#include <chrono>
#include <atomic>
#include <thread>


namespace FISIR {

	// 特化检查函数
	template<typename T>
	inline bool QueTest(const T& val) {
		return true;  // 默认返回 true
	}
	
	template<typename T, size_t N = 256>
	class LockFreeQue {
		static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two and greater than 0");
		static constexpr size_t mask_ = N - 1;

		struct alignas(64) Node {
			std::atomic_size_t sequence {0};
			T data;
		};

		struct alignas(64) ProducerState {
			std::atomic_size_t sequence {0};
			std::atomic_size_t cacheConsumer{ 0 };
			char padding[64 - sizeof(std::atomic_size_t) * 2]; // 填充到 64 字节
		} producer;

		struct alignas(64) ConsumerState {
			std::atomic_size_t sequence{ 0 };
			std::atomic_size_t cacheProducer{ 0 };
			char padding[64 - sizeof(std::atomic_size_t) * 2]; // 填充到 64 字节
		} consumer;

		alignas(64) Node buffer_[N];
		
	public:
		LockFreeQue() {
			for (size_t i = 0; i < N; ++i) {
				buffer_[i].sequence.store( i, std::memory_order_release);
			}

			producer.sequence.store(0);
			producer.cacheConsumer.store(0);
			consumer.sequence.store(0);
			consumer.cacheProducer.store(0);
			stop_.store(false, std::memory_order_relaxed);
			Size.store(0, std::memory_order_relaxed);

		}

		LockFreeQue(const LockFreeQue&) = delete;
		LockFreeQue& operator=(const LockFreeQue&) = delete;

		LockFreeQue(LockFreeQue&&) = delete;
		LockFreeQue& operator=(LockFreeQue&&) = delete;

		~LockFreeQue() {
			stop_.store(true, std::memory_order_release);

			for (int i=0; i<1024; i++) sem_.release();

		}

		bool push(const T& val) {
			return push_internal(val);
		}

		bool push(T&& val) {
			return push_internal(std::move(val));
		}

		template<class... Args>
		bool emplace(Args&&... args) {
			return push_internal(T(std::forward<Args>(args)...));
		}

		bool pop() {
			T dummy;
			return pop(dummy);
		}

		bool pop(T& ret) {
			return pop_internal(ret);
		}

		bool pop_wait(T& ret) {
			while(!stop_.load(std::memory_order_acquire)) {
				if (pop(ret)) return true;
				if (empty()) {
					sem_.try_acquire_for(std::chrono::milliseconds(1));
				}
			}
			return false;
		}

		void forceClear() {
			stop_.store(true, std::memory_order_release);
			for (int i = 0; i < 1024; i++) sem_.release();


		}

		size_t size() const {return Size;}

		bool empty() const {
			return producer.sequence.load(std::memory_order_acquire)
				- consumer.sequence.load(std::memory_order_acquire) == 0;
		}

		void clear() {
			for (size_t i = 0; i < N; ++i) {
				buffer_[i].sequence.store(i, std::memory_order_release);
			}
			producer.sequence.store(0, std::memory_order_release);
			producer.cacheConsumer.store(0, std::memory_order_release);
			consumer.sequence.store(0, std::memory_order_release);
			consumer.cacheProducer.store(0, std::memory_order_release);
			Size.store(0, std::memory_order_release);
		}

		void stopQue() {
			stop_.store(true, std::memory_order_release);
			for (int i = 0; i < 1024; ++i) {
				sem_.release();
			}
		}

	private:
		template<class U>
		bool push_internal(U&& val) {
			size_t prod_seq = producer.sequence.load(std::memory_order_relaxed);
			size_t cons_seq;

			if (prod_seq - producer.cacheConsumer.load(std::memory_order_acquire) >= N) {
				cons_seq = consumer.sequence.load(std::memory_order_acquire);
				producer.cacheConsumer.store(cons_seq, std::memory_order_release);
				if (prod_seq - cons_seq >= N) return false;
			}

			while(!producer.sequence.compare_exchange_weak(prod_seq, prod_seq + 1, std::memory_order_release, std::memory_order_relaxed)) {
				cons_seq = consumer.sequence.load(std::memory_order_acquire);
				if (prod_seq - cons_seq >= N) return false;
			}

			size_t index = prod_seq & mask_;

			//if (index == 0) __debugbreak();
		
			while(buffer_[index].sequence.load(std::memory_order_acquire) != prod_seq ) {
				if (stop_.load(std::memory_order_acquire)) {
					producer.sequence.store(prod_seq, std::memory_order_release);
					return false;
				}
				std::this_thread::yield();  // 避免忙等待
			}

			buffer_[index].data = std::forward<U>(val);
			buffer_[index].sequence.store(prod_seq + 1, std::memory_order_release);
			Size.fetch_add(1, std::memory_order_release);
			sem_.release();

			return true;
		}

		bool pop_internal(T& ret) {
			size_t cons_seq = consumer.sequence.load(std::memory_order_relaxed);
			size_t prod_seq;

			if (cons_seq >= consumer.cacheProducer.load(std::memory_order_acquire)) {
				prod_seq = producer.sequence.load(std::memory_order_acquire);
				consumer.cacheProducer.store(prod_seq, std::memory_order_release);
				if (cons_seq >= prod_seq) return false;
			}

			while(!consumer.sequence.compare_exchange_weak(cons_seq, cons_seq + 1, std::memory_order_release, std::memory_order_relaxed)) {
				prod_seq = producer.sequence.load(std::memory_order_acquire);
				if (cons_seq >= prod_seq) return false;
			}

			size_t index = cons_seq & mask_;


			while(buffer_[index].sequence.load(std::memory_order_acquire) != cons_seq + 1) {
				if (stop_.load(std::memory_order_acquire)) {
					consumer.sequence.store(cons_seq, std::memory_order_release);
					return false;
				}
				std::this_thread::yield();  // 避免忙等待

			}

			ret = std::move(buffer_[index].data);
			buffer_[index].sequence.store(cons_seq + N, std::memory_order_release);
			Size.fetch_sub(1, std::memory_order_release);
			//if (index == 0) __debugbreak();
			if (index == 0) tmp.fetch_add(1);
			return true;
		}
	
	private:
		alignas(64) std::binary_semaphore sem_{ 0 };          
		alignas(64) std::atomic_bool stop_{ false };         
		std::atomic_size_t Size{ 0 };
		std::atomic_size_t tmp {0};
	};
}