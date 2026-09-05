#include <CharacterEmbedding.h>

#include <onnxruntime_c_api.h>

#include <logger.h>

#include <array>
#include <cmath>
#include <fstream>
#include <sstream>

#include <Windows.h>

// The runtime half of CharacterEmbedding: binding to SkyrimNet's ONNX
// Runtime and running the model.
//
// Separate translation unit from the tokenizer so the tokenizer stays
// drivable without any of this.
namespace NarrativeEngine::CharacterEmbedding
{
    namespace
    {
        // SkyrimNet's own copy, already loaded in the process by its
        // embedding service. GetModuleHandle rather than LoadLibrary:
        // attaching to what is there touches no file and changes no
        // reference count. LoadLibrary is the fallback for the case
        // where SkyrimNet has not initialised its own service yet.
        constexpr const char* kRuntimeModule = "onnxruntime_skyrimnet.dll";
        constexpr const char* kRuntimePath = "Data/SKSE/Plugins/SkyrimNet/libs/onnxruntime_skyrimnet.dll";

        constexpr const wchar_t* kModelFile = L"all-MiniLM-L6-v2.onnx";
        constexpr const char* kVocabFile = "all-MiniLM-L6-v2-tokenizer.json";

        // The model's own names for its tensors, read off the file
        // rather than assumed. `sentence_embedding` is already
        // mean-pooled over the tokens, which is why nothing here pools:
        // the other output, `token_embeddings`, would need it.
        constexpr const char* kInputNames[]{"input_ids", "attention_mask", "token_type_ids"};
        constexpr const char* kOutputNames[]{"sentence_embedding"};

        const OrtApi* g_api = nullptr;
        OrtEnv* g_env = nullptr;
        OrtSession* g_session = nullptr;
        OrtSessionOptions* g_options = nullptr;
        OrtMemoryInfo* g_memory = nullptr;
        Vocabulary g_vocab;
        bool g_ready = false;

        std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in{path, std::ios::binary};
            if (!in) {
                return {};
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        // True when the call succeeded. ONNX Runtime reports failure by
        // returning a status object that the caller then owns.
        bool Succeeded(OrtStatus* status, const char* what)
        {
            if (status == nullptr) {
                return true;
            }
            logger::warn("CharacterEmbedding: {} failed: {}", what, g_api->GetErrorMessage(status));
            g_api->ReleaseStatus(status);
            return false;
        }
    } // namespace

    bool Initialize(const std::filesystem::path& modelDirectory)
    {
        if (g_ready) {
            return true;
        }

        HMODULE runtime = ::GetModuleHandleA(kRuntimeModule);
        if (runtime == nullptr) {
            runtime = ::LoadLibraryA(kRuntimePath);
        }
        if (runtime == nullptr) {
            logger::info("CharacterEmbedding: SkyrimNet's ONNX Runtime is not loaded; retrieval stays lexical.");
            return false;
        }

        using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
        auto* getBase =
            reinterpret_cast<GetApiBaseFn>(reinterpret_cast<void*>(::GetProcAddress(runtime, "OrtGetApiBase")));
        if (getBase == nullptr) {
            logger::warn("CharacterEmbedding: the runtime exports no OrtGetApiBase.");
            return false;
        }

        const OrtApiBase* base = getBase();
        // A runtime serves its own API version and every older one, so
        // this only fails if SkyrimNet ever ships one OLDER than the
        // header this was built against. Worth saying plainly rather
        // than crashing on a null table.
        g_api = base != nullptr ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (g_api == nullptr) {
            logger::warn("CharacterEmbedding: runtime {} does not serve API version {}; retrieval stays lexical.",
                         base != nullptr ? base->GetVersionString() : "(unknown)",
                         ORT_API_VERSION);
            return false;
        }

        const auto vocabJson = ReadFile(modelDirectory / kVocabFile);
        if (vocabJson.empty() || !g_vocab.LoadFromJson(vocabJson)) {
            logger::info("CharacterEmbedding: no usable tokenizer at {}; retrieval stays lexical.",
                         (modelDirectory / kVocabFile).string());
            return false;
        }

        const auto modelPath = modelDirectory / kModelFile;
        std::error_code ec;
        if (!std::filesystem::exists(modelPath, ec)) {
            logger::info("CharacterEmbedding: no model at {}; retrieval stays lexical.", modelPath.string());
            return false;
        }

        if (!Succeeded(g_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "NarrativeEngine", &g_env), "CreateEnv")
            || !Succeeded(g_api->CreateSessionOptions(&g_options), "CreateSessionOptions")) {
            Shutdown();
            return false;
        }
        // Let the runtime use the machine.
        //
        // This was pinned to one thread, on the reasoning that competing
        // with the engine during a load was the greater cost. That was
        // the wrong trade: a pass measured 1.2 ms on a quiet machine and
        // 8 ms during an actual load, turning 5,558 blocks into
        // forty-five seconds of stall. Left to itself the runtime is
        // several times faster, and with the cache below the whole cost
        // only lands once per save anyway.

        if (!Succeeded(g_api->CreateSession(g_env, modelPath.c_str(), g_options, &g_session), "CreateSession")
            || !Succeeded(g_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_memory),
                          "CreateCpuMemoryInfo")) {
            Shutdown();
            return false;
        }

        g_ready = true;
        logger::info("CharacterEmbedding: bound to ONNX Runtime {}, vocabulary {} tokens.",
                     base->GetVersionString(),
                     g_vocab.Size());
        return true;
    }

    void Shutdown()
    {
        if (g_api != nullptr) {
            if (g_memory != nullptr) {
                g_api->ReleaseMemoryInfo(g_memory);
            }
            if (g_session != nullptr) {
                g_api->ReleaseSession(g_session);
            }
            if (g_options != nullptr) {
                g_api->ReleaseSessionOptions(g_options);
            }
            if (g_env != nullptr) {
                g_api->ReleaseEnv(g_env);
            }
        }
        g_memory = nullptr;
        g_session = nullptr;
        g_options = nullptr;
        g_env = nullptr;
        g_ready = false;
    }

    bool IsAvailable()
    {
        return g_ready;
    }

    std::vector<float> Embed(std::string_view text)
    {
        if (!g_ready) {
            return {};
        }
        auto ids = g_vocab.Tokenize(text);
        // [CLS] and [SEP] alone carry no content.
        if (ids.size() <= 2) {
            return {};
        }

        const std::vector<std::int64_t> mask(ids.size(), 1);
        const std::vector<std::int64_t> types(ids.size(), 0);
        const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(ids.size())};
        const std::vector<std::int64_t>* sources[]{&ids, &mask, &types};

        std::array<OrtValue*, 3> inputs{};
        bool built = true;
        for (std::size_t i = 0; i < inputs.size() && built; ++i) {
            built = Succeeded(g_api->CreateTensorWithDataAsOrtValue(g_memory,
                                                                    const_cast<std::int64_t*>(sources[i]->data()),
                                                                    sources[i]->size() * sizeof(std::int64_t),
                                                                    shape.data(),
                                                                    shape.size(),
                                                                    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                                    &inputs[i]),
                              "CreateTensorWithDataAsOrtValue");
        }

        std::vector<float> vector;
        OrtValue* output = nullptr;
        if (built
            && Succeeded(
                g_api->Run(g_session, nullptr, kInputNames, inputs.data(), inputs.size(), kOutputNames, 1, &output),
                "Run")
            && output != nullptr) {
            float* raw = nullptr;
            if (Succeeded(g_api->GetTensorMutableData(output, reinterpret_cast<void**>(&raw)), "GetTensorMutableData")
                && raw != nullptr) {
                vector.assign(raw, raw + kDimensions);
                // Normalised here, once, so every later comparison is a
                // dot product rather than a cosine with two square roots
                // in it. Similarity() depends on this.
                double norm = 0.0;
                for (const auto x : vector) {
                    norm += static_cast<double>(x) * static_cast<double>(x);
                }
                norm = std::sqrt(norm);
                if (norm > 0.0) {
                    for (auto& x : vector) {
                        x = static_cast<float>(static_cast<double>(x) / norm);
                    }
                } else {
                    vector.clear();
                }
            }
        }

        if (output != nullptr) {
            g_api->ReleaseValue(output);
        }
        for (auto* tensor : inputs) {
            if (tensor != nullptr) {
                g_api->ReleaseValue(tensor);
            }
        }
        return vector;
    }
} // namespace NarrativeEngine::CharacterEmbedding
