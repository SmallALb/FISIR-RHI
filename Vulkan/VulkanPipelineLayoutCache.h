static std::unordered_map<PieplineLayoutHash, VkPipelineLayout> PipelineLayoutMap;
static std::unordered_map<RHIPipelineState, VulkanPipeline*> PipelineCacheMap;

//EXPORT FUNC//


std::unordered_map<PieplineLayoutHash, VkPipelineLayout>& getPipelineLayoutMap() {
    return PipelineLayoutMap;
}

// 管线缓存也走访问器：RHI 析构时要**清空**它（只 delete 元素不清表的话，同一个进程里第二次
// 初始化会命中悬垂的 VulkanPipeline* —— Android 上窗口被回收后重建走的就是这条路）。
std::unordered_map<RHIPipelineState, VulkanPipeline*>& getPipelineCacheMap() {
    return PipelineCacheMap;
}