#pragma once

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include <slang.h>

#include "Log/Logger.h"
#include "RHICreator.h"

// ─────────────────────────────────────────────────────────────────────────────
// 着色器编译器：Slang SDK（替代原先的 HLSL + DXC + dxcompiler.dll 动态加载）
//
// 为什么换 Slang：
//   · 一套编译器同时吃 HLSL 语法与 Slang 语言，且直接产出 SPIR-V（不必再走 DXC 的 -spirv）；
//   · 64 位整型原子（RWByteAddressBuffer/Load/Interlocked 的 64 位形式）由 Slang 自己按需
//     提升能力集，会生成 OpCapability Int64Atomics，Nanite 的「64 位打包原子」才成立；
//   · 绑定不再依赖「register 号恰好互不冲突」的隐式推断 —— 着色器里显式写 [[vk::binding]]。
//
// 与 DXC 时代保持一致的对外接口：仍是 compileShader(源串, 大小, 入口点名, 目标串)，
// 目标串（cs_6_7 / vs_5_0 / ps_6_0 …）在这里只用来取**着色阶段**，产物一律是 SPIR-V 1.5。
// 也就是说：调用方（Vulkan RHI、各示例、ImGui 后端）不需要改一行。
// ─────────────────────────────────────────────────────────────────────────────

namespace FISIR {

    // ── Slang 运行时的全局状态 ────────────────────────────────────────────
    // global session 进程内唯一（会话/模块都由它派生），引用计数归零时才释放。
    inline std::atomic_bool slangLoaded = false, needRelease = false;
    inline std::atomic_int32_t ComplierCount = 0;
    inline std::mutex initMutex;
    // Slang 的 global session / session **不是线程安全的**（载入模块、类型检查、特化、链接都不是
    // 可重入的；只有「已特化并链接完成」的组件可以并发取码）。这个工程里的编译都发生在初始化
    // 阶段的主线程上，但 Vulkan RHI 与示例可能各自持有 ShaderComplier，所以这里用一把互斥锁
    // 把所有 Slang API 调用串起来，避免多线程同时编译时踩到未定义行为。
    inline std::mutex slangMutex;
    inline slang::IGlobalSession* SlangGlobalSession = nullptr;
    // 每次编译给模块取一个唯一名字：同一会话内**同名模块会被缓存复用**，
    // 复用会让「同一份源、换一个入口点再编一次」拿到上一次的模块（NaniteRender 就是这样：
    // 一个文件要编出 mainRender / mainVS / mainPS 三个入口）。
    inline std::atomic_uint32_t ShaderModuleSerial = 0;

    struct ShaderComplierData {
        slang::ISession* session = nullptr;
    };

    // UTF-16 → UTF-8。Slang 的源码入口只吃 UTF-8 字节串，而 C++ 侧的内嵌着色器是宽字符串。
    inline std::string WideToUTF8(const wchar_t* data, size_t charCount) {
        if (!data || charCount == 0) return {};
#ifdef _WIN32
        const int len = WideCharToMultiByte(CP_UTF8, 0, data, (int)charCount, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string result((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, data, (int)charCount, result.data(), len, nullptr, nullptr);
        return result;
#else
        std::string result;
        result.reserve(charCount);
        for (size_t i = 0; i < charCount; ++i) {
            const unsigned int cp = (unsigned int)(unsigned short)data[i];
            if (cp < 0x80) result.push_back((char)cp);
            else if (cp < 0x800) {
                result.push_back((char)(0xC0 | (cp >> 6)));
                result.push_back((char)(0x80 | (cp & 0x3F)));
            }
            else {
                result.push_back((char)(0xE0 | (cp >> 12)));
                result.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                result.push_back((char)(0x80 | (cp & 0x3F)));
            }
        }
        return result;
#endif
    }

    // 目标串（"cs_6_7" / "ps_5_0" …）→ Slang 的着色阶段。
    // 版本号对 SPIR-V 没有意义（能力集由 Slang 按用到的特性自己提），只有前缀有用。
    inline SlangStage StageFromTarget(const char* target) {
        if (target && target[0] && target[1]) {
            switch ((target[0] << 8) | target[1]) {
            case ('c' << 8) | 's': return SLANG_STAGE_COMPUTE;
            case ('v' << 8) | 's': return SLANG_STAGE_VERTEX;
            case ('p' << 8) | 's': return SLANG_STAGE_FRAGMENT;
            case ('g' << 8) | 's': return SLANG_STAGE_GEOMETRY;
            case ('h' << 8) | 's': return SLANG_STAGE_HULL;
            case ('d' << 8) | 's': return SLANG_STAGE_DOMAIN;
            case ('m' << 8) | 's': return SLANG_STAGE_MESH;
            case ('a' << 8) | 's': return SLANG_STAGE_AMPLIFICATION;
            default: break;
            }
        }
        Warn("Unknown shader target '{}', assuming compute", target ? target : "<null>");
        return SLANG_STAGE_COMPUTE;
    }

    // 打印并释放 Slang 的诊断 blob（失败原因都在这块文本里）。
    inline void LogSlangDiagnostics(const char* what, slang::IBlob*& diagnostics) {
        if (diagnostics && diagnostics->getBufferSize() > 0) {
            const std::string text((const char*)diagnostics->getBufferPointer(), diagnostics->getBufferSize());
            Error("Slang {} failed:\n{}", what, text);
        }
        else {
            Error("Slang {} failed (no diagnostics)", what);
        }
        if (diagnostics) {
            diagnostics->release();
            diagnostics = nullptr;
        }
    }

    // 造一个「HLSL/Slang 源码 → SPIR-V 1.5」的会话。每份 ShaderComplier 一个会话，
    // 与原先「一份 ShaderComplier 一个 DXC 编译器实例」的粒度一致。
    inline slang::ISession* CreateSlangSession(slang::IGlobalSession* globalSession) {
        slang::TargetDesc target{};
        target.format = SLANG_SPIRV;
        target.profile = globalSession->findProfile("spirv_1_5");
        // 直接把 SPIR-V 交给后端，不绕一圈 GLSL（等价 slangc 的 -emit-spirv-directly）。
        target.flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY;

        // 会话级编译选项。数组留足空位，具体条数走 optionCount。
        slang::CompilerOptionEntry options[2]{};
        uint32_t optionCount = 0;

        // Vulkan 管线用 shader->getEntryPoint() 当 VkPipelineShaderStageCreateInfo::pName，
        // 而 Slang 默认把 SPIR-V 的入口点名写成 "main" —— 必须打开这个开关，
        // 让 SPIR-V 里的入口点名 = 源码里的函数名（mainRender / mainVS / …），否则管线创建会失败。
        options[optionCount].name = slang::CompilerOptionName::VulkanUseEntryPointName;
        options[optionCount].value.kind = slang::CompilerOptionValueKind::Int;
        options[optionCount].value.intValue0 = 1;
        ++optionCount;

        if (RHICreator::getCrrentRenderInterfaceApi() == FISIR::RHIAPI::Vulkan &&
            getenv("FISIR_SPV_DEBUG_INFO")) {
            // 等价 DXC 时代的 -fspv-debug=vulkan-with-source：让 Nsight/RenderDoc 能显示源码。
            // Slang 只有「级别」没有「风味」，STANDARD（-g2）是官方文档里验证过 RenderDoc 的档位；
            // 打开后 SPIR-V 会带 NonSemantic.Shader.DebugInfo.100 扩展指令。
            //
            // 默认**关闭**：调试信息会显著抬高编译期的内存与时间（每份源码都要带上行号/类型信息，
            // 且这些指令全程留在 IR 里），而它只在用图形调试器时才需要 —— 需要时设
            // FISIR_SPV_DEBUG_INFO=1 即可。
            options[optionCount].name = slang::CompilerOptionName::DebugInformation;
            options[optionCount].value.kind = slang::CompilerOptionValueKind::Int;
            options[optionCount].value.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD;
            ++optionCount;
        }

        slang::SessionDesc sessionDesc{};
        sessionDesc.targets = &target;
        sessionDesc.targetCount = 1;
        // HLSL/DXC 的默认矩阵布局是列主序，C++ 侧的 GLM 也是列主序存进 cbuffer。
        // SessionDesc 的默认值是**行主序**，这里必须显式写死，否则矩阵会整体转置。
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
        sessionDesc.compilerOptionEntries = options;
        sessionDesc.compilerOptionEntryCount = optionCount;

        slang::ISession* session = nullptr;
        const SlangResult result = globalSession->createSession(sessionDesc, &session);
        if (SLANG_FAILED(result) || !session) {
            Error("Failed to create Slang session, Error: 0x{:x}", (size_t)result);
            return nullptr;
        }
        return session;
    }

    class ShaderComplier {
    public:
        static inline bool InitCompiler() {
            if (!slangLoaded) {
                std::lock_guard<std::mutex> lock(initMutex);
                if (!slangLoaded) {
                    slang::IGlobalSession* globalSession = nullptr;
                    // 语言版本必须显式指定：SlangGlobalSessionDesc 的默认值是
                    // SLANG_LANGUAGE_VERSION_2025，而 2025 语义要求每个模块顶部写 module 声明、
                    // 且默认 internal —— 本工程的着色器是 HLSL 风格的单文件片段，必须走 LEGACY
                    // （= SLANG_LANGUAGE_VERSION_DEFAULT，与 slangc 的默认行为一致）。
                    // 注意 SlangGlobalSessionDesc 是**全局命名空间**里的类型（不在 slang:: 下）。
                    SlangGlobalSessionDesc globalSessionDesc{};
                    globalSessionDesc.minLanguageVersion = SLANG_LANGUAGE_VERSION_LEGACY;
                    const SlangResult result = slang::createGlobalSession(&globalSessionDesc, &globalSession);
                    if (SLANG_FAILED(result) || !globalSession) {
                        Error("Failed to create Slang global session, Error: 0x{:x}", (size_t)result);
                    }
                    else {
                        SlangGlobalSession = globalSession;
                        slangLoaded = true;
                        const char* buildTag = globalSession->getBuildTagString();
                        Info("Slang global session created: {}", buildTag ? buildTag : "unknown");
                    }
                }
            }
            return slangLoaded;
        }

        static inline void DestroyCompiler() {
            needRelease = true;
            if (ComplierCount) return;
            std::lock_guard<std::mutex> lock(initMutex);
            if (SlangGlobalSession) {
                SlangGlobalSession->release();
                SlangGlobalSession = nullptr;
            }
            slangLoaded = false;
            needRelease = false;
        }

        ShaderComplier() {
            ComplierCount++;
            Info("Create ShaderComplier: {}", ComplierCount.load());

            InitCompiler();
            mData = new ShaderComplierData();
            if (!slangLoaded || !SlangGlobalSession) {
                Error("Slang is not available; shader compilation will fail");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(slangMutex);
                mData->session = CreateSlangSession(SlangGlobalSession);
            }
            if (!mData->session) Error("Slang session is unavailable, shader compilation will fail");
        }

        ~ShaderComplier() {
            // 先放会话再减计数：全局会话只有在「没有任何会话/编译器活着」时才能安全释放。
            if (mData) {
                if (mData->session) mData->session->release();
                delete mData;
                mData = nullptr;
            }
            ComplierCount--;
            // 最后一个编译器用完，就把**全局会话**也放掉。
            // 为什么必须这么做：Slang 的 global session 会连带 core module 与一大堆内部 arena，
            // 实测（Debug、TextureCube、含交换链）整个进程私有内存 925MB，而它只在初始化阶段用一次；
            // 更糟的是示例与 RHIVK 是两个二进制，各自有一份 inline 全局状态 ⇒ **两份 global session
            // 同时驻留**。这里让计数归零即释放，之后若还有编译需求 InitCompiler() 会重建
            // （代价是慢一点，换来的是常驻内存大幅下降）。
            if (needRelease || ComplierCount.load() == 0) {
                std::lock_guard<std::mutex> lock(initMutex);
                if (ComplierCount.load() == 0 && SlangGlobalSession) {
                    SlangGlobalSession->release();
                    SlangGlobalSession = nullptr;
                    slangLoaded = false;
                    Info("Slang global session released (no compiler alive)");
                }
                needRelease = false;
            }
            clear();
        }

        void clear() { delete[] shaderData; shaderData = nullptr; CodeSize = 0; }

        // 编译入口：源串（UTF-8）→ SPIR-V 字节码，结果留在 shaderData/CodeSize 里。
        void compileShader(const char* data, size_t dataSize, const char* entryPoint, const char* target) {
            if (shaderData) {
                Error("Shader data already exists, clear before compiling new shader");
                return;
            }
            if (!mData || !mData->session) {
                Error("Slang session is unavailable; cannot compile '{}'", entryPoint ? entryPoint : "<null>");
                return;
            }

            const std::string source(data ? data : "", dataSize);
            const std::string entry = (entryPoint && entryPoint[0]) ? entryPoint : "main";
            const SlangStage stage = StageFromTarget(target);
            const std::string moduleName = "FISIRShader" + std::to_string(ShaderModuleSerial.fetch_add(1));
            const std::string modulePath = moduleName + ".slang";

            // Slang 的载入/链接阶段不可重入，整段编译串行化（编译都发生在初始化阶段，代价可忽略）。
            std::lock_guard<std::mutex> lock(slangMutex);

            slang::IBlob* diagnostics = nullptr;
            slang::IModule* module = mData->session->loadModuleFromSourceString(
                moduleName.c_str(), modulePath.c_str(), source.c_str(), &diagnostics);
            if (!module) {
                LogSlangDiagnostics("load module", diagnostics);
                return;
            }

            // 入口点用 findAndCheckEntryPoint 显式带阶段：HLSL 语法里没有 [shader("...")] 标注，
            // 只靠名字推断不出 vs/ps/cs，调用方给的目标串是唯一来源。
            slang::IEntryPoint* entryPointObj = nullptr;
            SlangResult result = module->findAndCheckEntryPoint(entry.c_str(), stage, &entryPointObj, &diagnostics);
            if (SLANG_FAILED(result) || !entryPointObj) {
                LogSlangDiagnostics("find entry point", diagnostics);
                module->release();
                return;
            }

            slang::IComponentType* components[2] = { module, entryPointObj };
            slang::IComponentType* program = nullptr;
            result = mData->session->createCompositeComponentType(components, 2, &program, &diagnostics);
            if (SLANG_FAILED(result) || !program) {
                LogSlangDiagnostics("link entry point", diagnostics);
                entryPointObj->release();
                module->release();
                return;
            }

            // getEntryPointCode 要求组件**已完全特化并链接**，所以组合之后必须显式 link 一次
            // （Slang 文档的调用序列：module + entryPoint → createCompositeComponentType → link → 取码）。
            slang::IComponentType* linked = nullptr;
            result = program->link(&linked, &diagnostics);
            if (SLANG_FAILED(result) || !linked) {
                LogSlangDiagnostics("link program", diagnostics);
                program->release();
                entryPointObj->release();
                module->release();
                return;
            }

            slang::IBlob* code = nullptr;
            result = linked->getEntryPointCode(0, 0, &code, &diagnostics);
            if (SLANG_SUCCEEDED(result) && code && code->getBufferSize() > 0) {
                CodeSize = code->getBufferSize();
                shaderData = new unsigned char[CodeSize];
                memcpy(shaderData, code->getBufferPointer(), CodeSize);
                Debug("Shader '{}' compiled successfully: {} bytes / {} SPIR-V words", entry, CodeSize, CodeSize / 4);

                // 调试开关：设了 FISIR_DUMP_SPV=<目录> 就把产物落盘，方便用 slangc 复现/对比编译选项。
                if (const char* dumpDir = getenv("FISIR_DUMP_SPV")) {
                    const std::string dumpPath = std::string(dumpDir) + "/fisir_" + entry + ".spv";
                    if (FILE* f = fopen(dumpPath.c_str(), "wb")) {
                        fwrite(shaderData, 1, CodeSize, f);
                        fclose(f);
                        Info("SPIR-V dumped: {}", dumpPath);
                    }
                }
            }
            else {
                LogSlangDiagnostics("generate SPIR-V", diagnostics);
            }

            if (code) code->release();
            if (diagnostics) { diagnostics->release(); diagnostics = nullptr; }
            linked->release();
            program->release();
            entryPointObj->release();
            module->release();
        }

        // 便捷重载：接受 UTF-16 源（内置在 C++ 里的着色器字符串），内部转 UTF-8 后编译。
        void compileShader(const wchar_t* data, size_t dataSize, const wchar_t* entryPoint, const wchar_t* target) {
            const std::string utf8Source = WideToUTF8(data, dataSize / sizeof(wchar_t));
            const std::string utf8Entry = WideToUTF8(entryPoint, entryPoint ? wcslen(entryPoint) : 0);
            const std::string utf8Target = WideToUTF8(target, target ? wcslen(target) : 0);
            compileShader(utf8Source.data(), utf8Source.size(), utf8Entry.c_str(), utf8Target.c_str());
        }

        unsigned char* getShaderData() const { return shaderData; }
        size_t getShaderDataSize() const { return CodeSize; }

    private:
        unsigned char* shaderData = nullptr;
        size_t CodeSize = 0;
        ShaderComplierData* mData = nullptr;
    };

}
