#include "ClusterSelection.h"
#include "ShaderComplier.h"
#include <fstream>
#include <string>
#include <iterator>

// 从磁盘读取整个文件为字符串（二进制读取，保留原始 UTF-8 字节）
static std::string LoadFileText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Error("Failed to open shader file: {}", path);
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static FISIR::RHIShader* ClusterSelectionComputeShader = nullptr;
static FISIR::RHIBuffer* ClusterSelectionBuffer = nullptr;
static FISIR::RHIBuffer* ClusterDataBuffer = nullptr;  // outbuffer：簇选择结果
static FISIR::RHIPipeline* ClusterSelectionPipeline = nullptr;
static FISIR::RHIResourcePackResult ClusterSelectionResourcePack;
static FISIR::RHIFence* ClusterSelectionFence = nullptr;

void InitClusterSelection(FISIR::DynamicRHI* rhi) {
		//Complie and Create Shader（从磁盘加载 HLSL 源文件）
	std::string csSource = LoadFileText("Shader/ClusterSelection.hlsl");
	FISIR::ShaderComplier* csCompiler = new FISIR::ShaderComplier();
	csCompiler->compileShader(csSource.data(), csSource.size(), "mainCS", "cs_6_0");
	ClusterSelectionComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainCS", csCompiler->getShaderData(), csCompiler->getShaderDataSize());

	//Create Buffer
	FISIR::BufferInfo clusterSelectionBufferInfo{
		.data_CPU = nullptr,
		.size = 4 * 1024 * 1024,
		.bufferlayout = FISIR::RBuffer | FISIR::TransferDstBuffer,
		.memoryType = FISIR::MemTypeDeviceLocal,
	};

	ClusterSelectionBuffer = rhi->RHICreateBuffer(clusterSelectionBufferInfo);

	//Create OutBuffer
	FISIR::BufferInfo clusterDataBufferInfo{
		.data_CPU = nullptr,
		.size = 4 * 1024 * 1024,
		.bufferlayout = FISIR::RWBuffer | FISIR::TransferDstBuffer,
		.memoryType = FISIR::MemTypeDeviceLocal,
	};
	ClusterDataBuffer = rhi->RHICreateBuffer(clusterDataBufferInfo);

	ClusterSelectionResourcePack = rhi->RHICreateResourcePack({ ClusterSelectionBuffer, ClusterDataBuffer });

	//Create Descriptor and Pipeline
	FISIR::RHIPipelineDescribeInfo describeInfo {
		{0, 1, FISIR::RHIDescriptorTyp::RBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
	};


	FISIR::RHIPipelineState PipelineState {
		.describeInfo = describeInfo,
		.isComputePipeline = 1,
	};
	PipelineState.Shaders[FISIR::__COMPUTESHADER__] = ClusterSelectionComputeShader;
	ClusterSelectionPipeline = rhi->RHICreatePipeline(PipelineState);
	
	ClusterSelectionFence = rhi->RHICreateFence(false, "ClusterSelectionFence");
	if (!ClusterSelectionFence) Error("Failed to create ClusterSelectionFence");
}

void ExecuteClusterSelectionPass(FISIR::RHIComputeCommandList& Cmdlist) {
	Cmdlist.SetPipelineState(ClusterSelectionPipeline);
	Cmdlist.SetResourcePack(ClusterSelectionResourcePack);
	Cmdlist.dispatch(1, 1, 1);
	Cmdlist.End(ClusterSelectionFence);
	ClusterSelectionFence->wait();
	ClusterSelectionFence->reset();
}

void DestroyClusterResource(FISIR::DynamicRHI* rhi) {
	if (ClusterSelectionFence) {
		rhi->RHIDestroyFence(ClusterSelectionFence);
		ClusterSelectionFence = nullptr;
	}

	if (ClusterSelectionBuffer) {
		rhi->RHIDestroyBuffer(ClusterSelectionBuffer);
		ClusterSelectionBuffer = nullptr;
	}

	if (ClusterDataBuffer) {
		rhi->RHIDestroyBuffer(ClusterDataBuffer);
		ClusterDataBuffer = nullptr;
	}

	rhi->RHIDestroyResourcePack(ClusterSelectionResourcePack);
}

FISIR::RHIBuffer* GetClusterSelectionBuffer() {
	return ClusterSelectionBuffer;
}

FISIR::RHIBuffer* GetClusterDataBuffer() {
	return ClusterDataBuffer;
}

void SetClusterSelectionBuffer(FISIR::DynamicRHI* rhi) {
	//load File Data
	FILE* file = nullptr;
	size_t size = 0;
	fopen_s(&file, "Res/mitsuba.nanitemesh", "rb");
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	unsigned char* data = new unsigned char[size];
	fread(data, 1, size, file);
	fclose(file);
	
	//Update Buffer Data
	FISIR::BufferInfo tmpbufferinfo {
		.data_CPU = data,
		.size = size,
		.bufferlayout = FISIR::TransferSrcBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostCoherent | FISIR::MemTypHostVisable),
	};
	auto tmpBuffer = rhi->RHICreateBuffer(tmpbufferinfo);

	auto fence = rhi->RHICreateFence(false, "Resource Copy");

	FISIR::RHITransferCommandList transferList(rhi);
	transferList.CopyToBuffer(tmpBuffer, ClusterSelectionBuffer, 0, 0, size);
	transferList.End(fence);

	fence->wait();

	rhi->RHIDestroyBuffer(tmpBuffer);
	rhi->RHIDestroyFence(fence);

	delete[] data;
}

FISIR::RHIResourcePackResult& GetClusterSelectionResourcePack(){
	return ClusterSelectionResourcePack;
}
