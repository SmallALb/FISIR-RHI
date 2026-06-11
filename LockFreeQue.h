#pragma once
#include <optional>
#include <atomic>

namespace FISIR {
	
	template<typename T>
	class LockFreeQue {
		using Node_T = std::atomic<uintptr_t>;
		static constexpr uintptr_t TagMask = 0x3;
		static constexpr uintptr_t PtrMask = ~TagMask;
		Node_T nll = 0;

		struct Node {
			Node() {}
			
			template<class... Args>
			explicit Node(Args&&... args) : val(std::forward<Args>(args)...) {}
			T val;
			Node_T nxt{0};
		};
	public:
		LockFreeQue() {
			Node* node = new Node();
			tail_.store(createNode(node));
			head_.store(createNode(node));
		}

		LockFreeQue(const LockFreeQue&) = delete;
		LockFreeQue& operator=(const LockFreeQue&) = delete;

		

		LockFreeQue(LockFreeQue&& other) {
			clear();
			head_(other.head_.exchange(nullptr, std::memory_order_acq_rel)),
			tail_(other.tail_.exchange(nullptr, std::memory_order_acq_rel)), Size(other.Size.load());
		}
		LockFreeQue& operator=(LockFreeQue&& other) {
			if (this != &other) {
				clear();
				head_(other.head_.exchange(nullptr, std::memory_order_acq_rel)),
				tail_(other.tail_.exchange(nullptr, std::memory_order_acq_rel)), Size(other.Size.load());
			}
			return *this;
		}

		~LockFreeQue() {
			if (!getPtr(head_.load())) return;
			clear();
			delete getPtr(head_.load());
		}

		void push(const T& val) {
			Node* newNode = new Node(val);
			push_Node(createNode(newNode));
		}

		void push(T&& val) {
			Node* newNode = new Node(std::move(val));
			push_Node(createNode(newNode));
		}

		template<class... Args>
		void emplace(Args&&... args) {
			Node* newNode = new Node(std::forward<Args>(args)...);
			push_Node(createNode(newNode));
		}

		bool pop() {
			T dummy;
			return pop(dummy);
		}

		bool pop(T& ret) {
			Node* OldHead = nullptr;
			Node* OldTail = nullptr;
			Node* GetNext = nullptr;
			
			uintptr_t  OldHead_T = nll;
			uintptr_t  OldTail_T = nll;
			uintptr_t  GetNext_T = nll;
			
			while (1) {
				OldHead_T = head_.load(std::memory_order_acquire);
				OldHead = getPtr(OldHead_T);
				uintptr_t OldTag = getTag(OldHead_T);

				OldTail_T = tail_.load(std::memory_order_acquire);
				OldTail = getPtr(OldTail_T);

				GetNext_T = OldHead->nxt.load(std::memory_order_acquire);
				GetNext = getPtr(GetNext_T);
				
				if (OldHead_T != head_.load(std::memory_order_acquire)) continue;

				if (OldHead == OldTail) {
					if (GetNext == nullptr) return false;
					auto NewTail_T = createNode(GetNext, getTag(OldTail_T) + 1);
					tail_.compare_exchange_weak(OldTail_T, NewTail_T, std::memory_order_release);
				}
				else {
					ret = std::move(GetNext->val);
					auto newHead_T = createNode(GetNext, OldTag + 1);
					if (head_.compare_exchange_weak(OldHead_T, newHead_T, std::memory_order_release)) {
						Size--;
						delete OldHead;
						return true;
					}
				}
			}
		}


		size_t size() const {return Size;}

		bool empty() const {
			Node_T headVal = head_.load(std::memory_order_acquire);
			Node* head = getPtr(headVal);
			Node_T nextVal = head->nxt.load(std::memory_order_acquire);
			return getPtr(nextVal) == nullptr;
		}

		void clear() {
			T tmp;
			while(pop(tmp));
		}

	private:
		void push_Node(uintptr_t NewNode_T) {
			Node* OldTail = nullptr;
			Node* CurrentNxt = nullptr;

			uintptr_t  OldTail_T = nll;
			uintptr_t  CurrentNxt_T = nll;

			while (1) {
				OldTail_T = tail_.load(std::memory_order_acquire);
				OldTail = getPtr(OldTail_T);
				uintptr_t OldTag = getTag(OldTail_T);

				CurrentNxt_T = OldTail->nxt.load(std::memory_order_acquire);
				CurrentNxt = getPtr(CurrentNxt_T);

				if (CurrentNxt == nullptr) {
					if (OldTail->nxt.compare_exchange_weak(CurrentNxt_T, NewNode_T, std::memory_order_release)) {
						auto NewTail_T = createNode(getPtr(NewNode_T), (OldTag + 1));
						tail_.compare_exchange_weak(OldTail_T, NewTail_T, std::memory_order_release);
						Size++;
						return;
					}
				}
				else tail_.compare_exchange_weak(OldTail_T, CurrentNxt_T, std::memory_order_release);
			}
		}

		inline uintptr_t createNode(Node* node, uintptr_t tag = 0) const {
			return reinterpret_cast<uintptr_t>(node) | (tag & TagMask);
		}
		
		inline Node* getPtr(uintptr_t node) const {
			return reinterpret_cast<Node*>(node & PtrMask);
		}

		inline uintptr_t getTag(uintptr_t node) const {
			return node & TagMask;
		}
	private:
		Node_T head_{0}, tail_{0};
		std::atomic<size_t> Size{0};
	};
}