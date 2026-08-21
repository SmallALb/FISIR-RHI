#include "DynamicRHI.h"
#include "RHIShader.h"
#include "RHIBuffer.h"
#include "RHIPipeline.h"
#include "RHICommandList.h"
#include "RHIFence.h"

void InitClusterSelection(FISIR::DynamicRHI* rhi);

void ExecuteClusterSelectionPass(FISIR::RHIComputeCommandList& Cmdlist);

void DestroyClusterResource(FISIR::DynamicRHI* rhi);


FISIR::RHIBuffer* GetClusterSelectionBuffer();

FISIR::RHIBuffer* GetClusterDataBuffer();

void SetClusterSelectionBuffer(FISIR::DynamicRHI* rhi);


FISIR::RHIResourcePackResult& GetClusterSelectionResourcePack();