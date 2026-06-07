#pragma once

namespace FISIR {
	enum RHIUsingStage : uint8_t {
		NoneStage = 0x0,
		VertexShaderStage = 0x01,
		FragmentShaderStage = 0x02,
		TessShaderStage = 0x04,
		ComputeShaderStage = 0x08,
		GeometryShaderStage = 0x10,
		PipelinTopStage = 0x20,
		PipelineBottomStage = 0x40,
		PipelineVertexInputStage = 0x80,
		PipelineBeforeFragmentStage = 0x100,
		PipelineAfterFragmentStage = 0x200,

	};

}