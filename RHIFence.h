#pragma once 

#include <cstdint>

namespace FISIR {

	class RHIFence {
	public:
		enum class Statue {
			Pendding,
			Signaled,
			UnSignaled,
		};

		virtual ~RHIFence() {};

		virtual void* getFenceHandle() const = 0;

		virtual Statue getFenceStage() const = 0;

		virtual void wait() = 0;

		virtual bool waitFor(uint64_t timeout = UINT64_MAX) = 0;

		virtual bool isSubmited() = 0;

		// 非阻塞查询「这一次提交的 GPU 工作是否已经跑完」（= 围栏已置位）。只查询，不等待。
		// 给**绝不能阻塞**的路径用：RHI 线程在取图时判断交换链槽位能否复用。本线程是所有提交的
		// 唯一执行者、也是所有取图请求的唯一回信人 —— 在那里等一个可能永不置位的围栏，会把
		// 取图回信和整条提交流水线一起焊死（窗口「未响应」，实测栈：
		// VulkanRHILoop → tryAcquire → VulkanFence::wait → vkWaitForFences(…, UINT64_MAX)）。
		virtual bool isSignaled() = 0;

		virtual bool waitFenceSubmited(uint64_t timeout = UINT64_MAX) = 0;

		virtual void reset() = 0;
	};

}