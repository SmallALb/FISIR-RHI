#pragma once

namespace FISIR {
	static uint64_t align_up(uint64_t size, uint64_t align) {
		return (size + align - 1) & ~(align - 1);
	}


	constexpr unsigned int FIRST_LEVEL_INDEX_MAX = 32;    //一级最大索引数
	constexpr unsigned int SECOND_LEVEL_INDEX_COUNT = 32; //单个一级的二级数量
	constexpr unsigned int FIRST_LEVEL_INDEX_SHIFT = 8; //计算大和小分界点的幂次
	constexpr unsigned int FIRST_LEVEL_INDEX_COUNT = FIRST_LEVEL_INDEX_MAX - FIRST_LEVEL_INDEX_SHIFT + 1;  //实际使用的一级数量（25）
	constexpr unsigned int SMALL_BLOCK_SIZE = 1 << FIRST_LEVEL_INDEX_SHIFT; //最小块大小



	struct GpuBlock {
		GpuBlock() {}

		GpuBlock(VkDeviceMemory mem, size_t Size = 0, size_t offset = 0, size_t lstblocksize = 0, GpuBlock* N = nullptr, GpuBlock* L = nullptr) :
			nxt(N), lst(L), lstBlockSize(lstblocksize)
		{
			Info.GpuMemory = mem;
			Info.Size = Size;
			Info.offset = offset;
		}
		BlockInfo Info;
		size_t lstBlockSize{ 0 };
		size_t PoolID{ 0 };
		bool isFreeBlock{ 1 };

		GpuBlock* nxt = nullptr;
		GpuBlock* lst = nullptr;
		RHIResource* bindingResource {nullptr};
	};


	struct AllocationPool {
		AllocationPool() : totalSize(0), MemTypeID(UINT32_MAX), UsingDevice(VK_NULL_HANDLE) {

		}


		AllocationPool(size_t ID, size_t Size, uint32_t MemTyp, VkDevice device, bool HeapSupported) :
			totalSize(Size), MemTypeID(MemTyp), UsingDevice(device), PoolID(ID) {
			VkMemoryAllocateFlagsInfo flagsInfo{
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
				.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
			};

			VkMemoryAllocateInfo info{
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = totalSize,
				.memoryTypeIndex = MemTypeID
			};
			if (HeapSupported) info.pNext = &flagsInfo;
			vkAllocateMemory(UsingDevice, &info, nullptr, &Pool);

			nllBlock = GpuBlock(Pool);
			nllBlock.nxt = &nllBlock;
			nllBlock.lst = &nllBlock;

			for (uint32_t f = 0; f < FIRST_LEVEL_INDEX_COUNT; f++) {
				for (uint32_t s = 0; s < SECOND_LEVEL_INDEX_COUNT; s++)
					blocks[f][s] = &nllBlock;
				Second_level[f] = 0;
			}

			offset_blocks[0] = new GpuBlock(Pool, totalSize);
			Push_List(offset_blocks[0]);
		}

		~AllocationPool() {
			for (auto& [offset, block] : offset_blocks)
				delete block;
			vkFreeMemory(UsingDevice, Pool, nullptr);
			
		}


		GpuBlock* NewBlock(size_t Size, uint64_t align) {
			size_t alignedSize = align_up(Size, align);
			if (alignedSize > totalSize) return nullptr;
			int f, s;
			mapping_insert(alignedSize, f, s);
			GpuBlock* block = getSuitable(f, s);
			if (!block) return nullptr;

			// ① 桶是**区间**映射，不是精确尺寸映射。大块路径下同一个 (f,s) 覆盖 2048 字节的区间
			// （例如 f=9 时 s = (Size>>11)&31，覆盖 [65536,131072) 分 32 桶、每桶 2048），
			// 所以同桶里拿到的块**可能比请求小**。原实现把这个判断藏在 SplitBlock 里、
			// 并且忽略它的返回值 ⇒ 把「不够大」的块当分配成功返回；调用方随后按**请求大小**
			// vkBindBufferMemory/vkBindImageMemory ⇒ 该资源的内存区间越过后面的块，
			// 与紧邻的活资源重叠 ⇒ 两个活资源落在同一段地址（描述符堆 VUID-11236/11228 的源头）。
			const size_t oldOffset     = block->Info.offset;
			const size_t alignedOffset = align_up(oldOffset, align);
			const size_t offsetDiff    = alignedOffset - oldOffset;
			const size_t newSize       = block->Info.Size - offsetDiff;
			if (newSize < alignedSize) return nullptr;

			// ② Remove_List 是**按 block->Info.Size 反推 (f,s)** 来定位链表的。原实现先按对齐
			// 改掉 Info.Size/offset 再摘链 ⇒ 摘的是**另一条**链表：真实桶的表头/位图仍指向这个块，
			// 该块随即被标记为已分配却依旧可达 ⇒ 下次 getSuitable 会把它再发一次（同址双分配）。
			// 必须**先摘、后改**。
			Remove_List(block);

			if (offsetDiff > 0) {
				// 对齐产生的前导空洞 [oldOffset, alignedOffset) 必须登记成空闲块。
				// 原实现直接把它丢掉 ⇒ offset_blocks 不再完整覆盖本池（合并失灵、内存永久泄漏），
				// 而且块的真实 offset 变了却没人搬 offset_blocks 的键。
				GpuBlock* front = new GpuBlock(Pool, offsetDiff, oldOffset, block->lstBlockSize);
				front->isFreeBlock    = 1;
				front->PoolID         = PoolID;
				front->Info.MemoryType = MemTypeID;
				offset_blocks[oldOffset]     = front;    // 复用原键
				offset_blocks[alignedOffset] = block;    // ★ 键跟着块的真实 offset 搬
				Push_List(front);
				block->lstBlockSize = offsetDiff;
			}
			block->Info.offset = alignedOffset;
			block->Info.Size   = newSize;

			block->PoolID = PoolID;
			block->isFreeBlock = 0;
			SplitBlock(block, alignedSize);
			block->Info.MemoryType = MemTypeID;
			totalSize -= alignedSize;
			UsingSize += alignedSize;
			return block;
		}

		void FreeBlock(GpuBlock* block) {
			if (!block) return;
			// ③ 原来**没有**重入防线：isFreeBlock 被写但从不被读。
			// 同一个块被 free 两次时，第二次会（a）再累加一次 totalSize/扣一次 UsingSize，
			// （b）对**已不在链表中**的块调用 MergeBack/Front 里的 Remove_List —— 那会用陈旧的
			// nxt/lst 把无关的空闲块从链表上摘掉，（c）把同一个块 Push_List 两次形成自环，
			// 之后 getSuitable 永远返回它 ⇒ 同址无限双分配。
			if (block->isFreeBlock) {
				Error("AllocationPool::FreeBlock: block at offset 0x{:x} (size {}) is already free -- double free",
					block->Info.offset, block->Info.Size, PoolID);
				return;
			}
			if (block->PoolID != PoolID) {
				Error("Block ID Not Equal the Pool ID");
				return;
			}
			int f, s;
			mapping_insert(block->Info.Size, f, s);
			totalSize += block->Info.Size;
			UsingSize -= block->Info.Size;
			block->isFreeBlock = 1;
			MergeBackBlock(block);
			MergeFrontBlock(block);
			Push_List(block);
		}

		void Remove_List(GpuBlock* block) {
			int f, s;
			mapping_insert(block->Info.Size, f, s);

			block->lst->nxt = block->nxt;
			block->nxt->lst = block->lst;

			if (blocks[f][s] == block) {
				blocks[f][s] = blocks[f][s]->nxt;
				if (blocks[f][s] == &nllBlock)
					Second_level[f] &= ~(1u << s);
				if (Second_level[f] == 0)
					First_level &= ~(1u << f);
			}
		}

		void Push_List(GpuBlock* block) {
			int f, s;
			mapping_insert(block->Info.Size, f, s);
			block->nxt = blocks[f][s];
			block->lst = &nllBlock;

			blocks[f][s]->lst = block;
			blocks[f][s] = block;

			if (blocks[f][s]->nxt == &nllBlock) nllBlock.nxt = block;

			First_level |= (1u << f);
			Second_level[f] |= (1u << s);
		}

		void mapping_insert(size_t Size, int& f, int& s) {
			if (Size < SMALL_BLOCK_SIZE) {
				f = 0;
				s = static_cast<int>(Size / (SMALL_BLOCK_SIZE / SECOND_LEVEL_INDEX_COUNT));
			}
			else {
				size_t round = Size;
				int fl = 0;
				while (round > 1) {
					round >>= 1;
					fl++;
				}
				f = fl - (FIRST_LEVEL_INDEX_SHIFT - 1);
				s = static_cast<int>(Size >> (fl - 5) & (SECOND_LEVEL_INDEX_COUNT - 1));
			}
		}

		int findfirstBit(uint32_t val) {
			if (val == 0) return -1;
			int bit = 0;
			while ((val & 1) == 0) {
				val >>= 1;
				bit++;
			}
			return bit;
		}

		GpuBlock* getSuitable(int f, int s) {
			int sl_map = Second_level[f] & (~0u << s);
			if (sl_map == 0) {
				uint32_t fl = First_level & (~0ull << (f + 1));
				if (!fl) return nullptr;
				f = findfirstBit(fl);
				sl_map = Second_level[f];
			}
			int sl = findfirstBit(sl_map);
			return blocks[f][sl];
		}

		GpuBlock* SplitBlock(GpuBlock* block, size_t Size) {
			if (block->Info.Size < Size) return nullptr;
			size_t remineSize = block->Info.Size - Size;
			size_t remineOffset = block->Info.offset + Size;
			block->Info.Size = Size;
			// ⑥ remineSize == 0（空闲块大小恰好等于请求）时**不能**造一个 0 字节的块：
			// offset_blocks 以 offset 为键，而 remineOffset 正是**后继块**的起始 offset，
			// 这个 0 字节块会把后继块的表项**覆盖掉**（原实现用 operator[] 直接赋值）⇒
			// 那个块从此在表里查不到，后继的合并/对齐查找全部失灵，最终表现为同址双分配。
			if (remineSize == 0) return nullptr;
			if (offset_blocks.contains(remineOffset)) {
				Error("offset 0x{:x} already has a block (corrupted free-block tiling)", remineOffset);
				return nullptr;
			}
			offset_blocks[remineOffset] = new GpuBlock(Pool, remineSize, remineOffset, Size);
			Push_List(offset_blocks[remineOffset]);
			return offset_blocks[remineOffset];
		}

		bool MergeBackBlock(GpuBlock* block) {
			size_t BackOffset = block->Info.Size + block->Info.offset;
			if (!offset_blocks.contains(BackOffset)) return false;
			if (!offset_blocks[BackOffset]->isFreeBlock) return false;
			// ④ 只能摘**要被合并掉的邻居**。原实现对正在释放、尚未入链的 block 也调了
			// Remove_List —— 它的 nxt/lst 还是上一轮分配前的陈旧链接，会把别的空闲块摘下来。
			Remove_List(offset_blocks[BackOffset]);
			auto mb = offset_blocks[BackOffset];
			offset_blocks.erase(BackOffset);
			block->Info.Size += mb->Info.Size;
			delete mb;
			return true;
		}

		bool MergeFrontBlock(GpuBlock* block) {
			if (block->Info.offset <= block->lstBlockSize) return false;
			size_t FrontOffset = block->Info.offset - block->lstBlockSize;
			if (!offset_blocks.contains(FrontOffset)) return false;
			if (!offset_blocks[FrontOffset]->isFreeBlock) return false;
			Remove_List(offset_blocks[FrontOffset]);
			auto mb = offset_blocks[FrontOffset];
			offset_blocks.erase(FrontOffset);
			const size_t oldOffset = block->Info.offset;
			block->Info.offset = mb->Info.offset;
			block->Info.Size += mb->Info.Size;
			block->lstBlockSize = mb->lstBlockSize;   // 合并后「前邻尺寸」必须跟着搬到更前面的那块
			// ⑤ offset_blocks **以 offset 为键**，块前并之后真实 offset 变了，键必须跟着搬。
			// 原实现漏了这一步 ⇒ 表的键与块的真实 offset 脱钩：后面对这段区间做
			// contains()/[] 查询或合并会命中**错的块**，于是同一段内存被发给两个活分配。
			if (oldOffset != block->Info.offset) {
				offset_blocks.erase(oldOffset);
				offset_blocks[block->Info.offset] = block;
			}
			delete mb;
			return true;
		}


		VkDeviceMemory Pool;
		VkDevice UsingDevice;
		size_t totalSize = 0;
		size_t UsingSize = 0;
		size_t PoolID = 0;
		uint32_t MemTypeID = UINT32_MAX;
		GpuBlock nllBlock;
		//offset + Size 如果存在，则可以合并
		sparse_map<size_t, GpuBlock*> offset_blocks;
		uint32_t First_level{ 0 };
		uint32_t Second_level[FIRST_LEVEL_INDEX_COUNT];
		GpuBlock* blocks[FIRST_LEVEL_INDEX_COUNT][SECOND_LEVEL_INDEX_COUNT];
	};

}