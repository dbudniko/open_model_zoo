/*
// Copyright (C) 2018-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "pipeline_base.h"
#include <samples/args_helper.hpp>
#include <cldnn/cldnn_config.hpp>

using namespace InferenceEngine;

PipelineBase::PipelineBase():
    inputFrameId(0),
    outputFrameId(0)
{
}

PipelineBase::~PipelineBase()
{
    waitForTotalCompletion();
}

void PipelineBase::init(const std::string& model_name, const CnnConfig& cnnConfig, InferenceEngine::Core* engine)
{
    auto ie = engine ?
        std::unique_ptr<InferenceEngine::Core, void(*)(InferenceEngine::Core*)>(engine, [](InferenceEngine::Core*) {}) :
        std::unique_ptr<InferenceEngine::Core, void(*)(InferenceEngine::Core*)>(new InferenceEngine::Core, [](InferenceEngine::Core* ptr) {delete ptr; });

    // --------------------------- 1. Load inference engine ------------------------------------------------
    slog::info << "Loading Inference Engine" << slog::endl;

    slog::info << "Device info: " << slog::endl;
    std::cout << ie->GetVersions(cnnConfig.devices);

    /** Load extensions for the plugin **/
    if (!cnnConfig.cpuExtensionsPath.empty()) {
        // CPU(MKLDNN) extensions are loaded as a shared library and passed as a pointer to base extension
        IExtensionPtr extension_ptr = make_so_pointer<IExtension>(cnnConfig.cpuExtensionsPath.c_str());
        ie->AddExtension(extension_ptr, "CPU");
    }
    if (!cnnConfig.clKernelsConfigPath.empty()) {
        // clDNN Extensions are loaded from an .xml description and OpenCL kernel files
        ie->SetConfig({ {PluginConfigParams::KEY_CONFIG_FILE, cnnConfig.clKernelsConfigPath} }, "GPU");
    }

    // --------------------------- 2. Read IR Generated by ModelOptimizer (.xml and .bin files) ------------
    slog::info << "Loading network files" << slog::endl;
    /** Read network model **/
    InferenceEngine::CNNNetwork cnnNetwork = ie->ReadNetwork(model_name);
    /** Set batch size to 1 **/
    slog::info << "Batch size is forced to 1." << slog::endl;
    cnnNetwork.setBatchSize(1);

    PrepareInputsOutputs(cnnNetwork);

    // --------------------------- 4. Loading model to the device ------------------------------------------
    slog::info << "Loading model to the device" << slog::endl;
    ExecutableNetwork execNetwork = ie->LoadNetwork(cnnNetwork, cnnConfig.devices, cnnConfig.execNetworkConfig);
    // -----------------------------------------------------------------------------------------------------

    // --------------------------- 5. Create infer requests ------------------------------------------------
    requestsPool = std::move(RequestsPool(execNetwork, cnnConfig.maxAsyncRequests));
}

void PipelineBase::waitForData()
{
    std::unique_lock<std::mutex> lock(mtx);

    condVar.wait(lock, [&] {return callbackException != nullptr || requestsPool.getInUseRequestsCount() || !completedRequestResults.empty(); });

    if (callbackException)
        std::rethrow_exception(callbackException);
}

int64_t PipelineBase::submitRequest(InferenceEngine::InferRequest::Ptr request)
{
    perfInfo.numRequestsInUse = (uint32_t)requestsPool.getInUseRequestsCount();

    if (outputName=="")
    {
        throw std::invalid_argument("outputName values is not set.");
    }
    auto frameStartTime = std::chrono::steady_clock::now();

    if (!perfInfo.startTime.time_since_epoch().count())
    {
        perfInfo.startTime = frameStartTime;
    }

    auto frameID = inputFrameId;

    request->SetCompletionCallback([this,
        frameStartTime,
        frameID,
        request] {
            {
                std::lock_guard<std::mutex> lock(mtx);

                try {
                    RequestResult result;

                    result.frameId = frameID;
                    result.output = std::make_shared<TBlob<float>>(*as<TBlob<float>>(request->GetBlob(this->outputName)));
                    result.startTime = frameStartTime;

                    completedRequestResults.emplace(frameID, result);

                    this->requestsPool.setRequestIdle(request);
                }
                catch (...) {
                    if (!this->callbackException) {
                        this->callbackException = std::move(std::current_exception());
                    }
                }
            }
            condVar.notify_one();
    });

    inputFrameId++;
    if(inputFrameId<0)
        inputFrameId=0;

    request->StartAsync();
    return frameID;
}

PipelineBase::RequestResult PipelineBase::getResult()
{
    std::lock_guard<std::mutex> lock(mtx);

    const auto& it = completedRequestResults.find(outputFrameId);
    if (it != completedRequestResults.end())
    {
        auto retVal = std::move(it->second);
        completedRequestResults.erase(it);
        outputFrameId++;
        if (outputFrameId < 0)
            outputFrameId = 0;

        // Updating performance info
        auto now = std::chrono::steady_clock::now();
        perfInfo.latencySum += (now - retVal.startTime);
        perfInfo.framesCount++;
        perfInfo.FPS = perfInfo.framesCount*1000.0 / std::chrono::duration_cast<std::chrono::milliseconds>(now - perfInfo.startTime).count();

        return retVal;
    }

    return RequestResult();
}
