#pragma once

namespace FISIR {
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


		GpuBlock* NewBlock(size_t Size) {
			if (Size > totalSize) return nullptr;
			int f, s;
			mapping_insert(Size, f, s);
			GpuBlock* block = getSuitable(f, s);
			if (!block) return nullptr;
			block->PoolID = PoolID;
			block->isFreeBlock = 0;
			Remove_List(block);
			SplitBlock(block, Size);
			block->Info.MemoryType = MemTypeID;
			totalSize -= Size;
			UsingSize += Size;
			Debug("Well New Block Success!");
			return block;
		}

		void FreeBlock(GpuBlock* block) {
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
			Debug("Well Free Block Success!");
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
			offset_blocks[remineOffset] = new GpuBlock(Pool, remineSize, remineOffset, Size);
			Push_List(offset_blocks[remineOffset]);
			return offset_blocks[remineOffset];
		}

		bool MergeBackBlock(GpuBlock* block) {
			size_t BackOffset = block->Info.Size + block->Info.offset;
			if (!offset_blocks.contains(BackOffset)) return false;
			if (!offset_blocks[BackOffset]->isFreeBlock) return false;
			Remove_List(block);
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
			Remove_List(block);
			Remove_List(offset_blocks[FrontOffset]);
			auto mb = offset_blocks[FrontOffset];
			offset_blocks.erase(FrontOffset);
			block->Info.offset = mb->Info.offset;
			block->Info.Size += mb->Info.Size;
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