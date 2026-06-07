static std::unordered_map<PieplineLayoutHash, VkPipelineLayout> PipelineLayoutMap;
static std::unordered_map<RHIPipelineState, VulkanPipeline*> PipelineCacheMap;

//EXPORT FUNC//


std::unordered_map<PieplineLayoutHash, VkPipelineLayout>& getPipelineLayoutMap() {
    return PipelineLayoutMap;
}