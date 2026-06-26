# RHICommand


RHICommand是枚举类型，记录了所有常用的Gpu命令

```cpp
enum class RHICommand {
	None = 0,
	//render
	BeginRenderPass,
	EndRenderPass,
	DrawPrimitive,
	DrawIndex,

	//Bind
	BindPipeline,
	BindVertexBuffer,
	BindIndexBuffer,
	BindResourceAndSamplerPack,

	//Transfer
	TransferTextures,
	TransferBuffers,
	CopyBufferToBuffer,
	CopyBufferToTexture,
};
```

同时对于每种命令都有其自己要提交的数据，这里将其打包为了结构体：
```cpp
struct BeginRenderPass_CmdInfo {
	RHIFrameBuffer* frame; 
	ClearValue value;
};

struct DrawPrimitive_CmdInfo {
	uint32_t BaseVertexIndex; 
	uint32_t NumsPrimitives; 
	uint32_t NumInstances;
};

struct DrawIndex_CmdInfo {
	uint32_t BaseVerterIndex;
	uint32_t IndexCount;
	uint32_t BaseInstanceIndex;
	uint32_t InsatnceCount;
};

struct BindViewPort_CmdInfo {
	float x; 
	float y; 
	float width; 
	float height; 
	float maxDepth; 
	float minDepth;
};

struct BindScissor_CmdInfo {
	uint32_t width;
	uint32_t height;
};

struct BindVertextBuffer_CmdInfo {
	RHIBuffer* buffer; 
	uint32_t binding; 
	uint64_t offset;
};

struct BindIndexBuffer_CmdInfo {
	RHIBuffer* buffer;
	uint64_t offset;
};

struct CopyBufferToBuffer_CmdInfo {
	RHIBuffer* src;
	RHIBuffer* dst;
	uint64_t srcOffset;
	uint64_t dstOffset;
	uint64_t size;
};

struct CopyBufferToTexture_CmdInfo {
	RHIBuffer* src;
	RHITexture* dst;
	uint32_t mipLevel;
	uint32_t arrayindex;
	uint32_t arraycount;
	uint64_t srcOffset;
	TextureSize dstOffset{};
	TextureSize dstSize;
};


struct Dispatch_CmdInfo {
	uint32_t GroupCountX; 
	uint32_t GroupCountY;
	uint32_t GroupCountZ;
};

struct BindPipeline_CmdInfo {
	RHIPipeline* pipeline;
};

struct ReserveInput_CmdInfo {
	uint64_t reserveData {0};
};
```
然后这些结构体会复制写入命令环形栈中：
```cpp
	FISIR::RingCommandStack::WriteData(RHICommandT commandT, const T& data);
```