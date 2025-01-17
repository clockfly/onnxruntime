// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <string>
#include <vector>
#include <unordered_map>

#include "core/framework/execution_provider.h"
#include "core/common/status.h"
#include "core/graph/basic_types.h"
#include "core/framework/ml_value.h"
#include "core/session/inference_session.h"
#include "core/common/logging/logging.h"

namespace onnxruntime {
class SessionState;
/**
 * Input/Output binding.
 * Usage is as follows:
 *
 * InferenceSession session;
 * session.Load();
 * session.Initialize();
 * ...
 * shared_ptr<IOBinding> io_binding;
 * session.NewIOBinding("DML", &io_binding);
 * io_binding->BindInput(...);
 * io_binding->BindInput(...);
 * io_binding->SynchronizeInputs();
 *
 * io_binding->BindOutput(...);
 * io_binding->BindOutput(...);
 *
 * session.Run(io_binding);
 *
 * vector<OrtValue>& outputs = io_binding->GetOutputs();
 */
class IOBinding {
 public:
  /**
   * Call repeatedly to bind as many inputs as required.
   * If called again for the same name will replace an existing value.
   * If the input ort_value is not at the desired location (specified by the execution provider), this will
   * copy it to the desired location. This copy may or may not be async. It depends on the exec provider.
   * If the input ort_value is not at the desired location, it should be preallocated
   * If the input ort_value isn't preallocated, it should have memtype of OrtMemTypeDefault
   * For copying it leverages DataTransferManager::CopyTensor().
   */
  common::Status BindInput(const std::string& name, const OrtValue& ml_value);

  /**
    * If the BindInput calls are async this function acts as a barrier to ensure all inputs are fully copied
    * before you call the Run() method. There is no point calling Run() if you're inputs are not ready at the 
    * desired location.
    * This is a blocking call and is a wrapper over IExecutionProvider::Sync().
    * Call InferenceSession::Run() only after calling this method or else you'll end up wasting cycles inside Run().
    */
  common::Status SynchronizeInputs();
  common::Status SynchronizeOutputs();

  /**
    * Bind an output name to a provided OrtValue. 
    * If the output is pre-allocated, the value in 'device' is not used.
    * If the output is not pre-allocated, information on the device it should be allocated on can be provided.
    * 
    * @param device Device to allocate on if not pre-allocated. Default is CPU. 
    */
  common::Status BindOutput(const std::string& name, const OrtValue& ml_value, OrtDevice device = {});

  /**
    * This simply collects the outputs obtained after calling Run() inside the @param outputs.
    */
  const std::vector<std::string>& GetOutputNames() const;
  std::vector<OrtValue>& GetOutputs();

  // device info for outputs that are not pre-allocated
  const std::vector<OrtDevice>& GetOutputsDeviceInfo() const;

  const std::vector<std::string>& GetInputNames() const;
  const std::vector<OrtValue>& GetInputs() const;

  /**
    * Get a CPU allocator from provider for async copy later if the provider supports that
    * If it doesn't support that, return the default allocator from CPU provider
    * \return a nonnull pointer
    */
  AllocatorPtr GetCPUAllocator(int id, onnxruntime::ProviderType provider_type) const;

  /**
    * clear inputs or outputs. IOBinding is stateful. There are cases we need to reset its state.
    */
  void ClearOutputs();
  void ClearInputs();

 private:
  friend InferenceSession;

  IOBinding(const SessionState& session_state);
  const SessionState& session_state_;
  std::vector<std::string> feed_names_;
  std::vector<OrtValue> feeds_;
  std::vector<std::string> output_names_;
  std::vector<OrtValue> outputs_;
  std::vector<OrtDevice> outputs_device_info_;

  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(IOBinding);
};
}  // namespace onnxruntime
