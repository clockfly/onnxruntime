// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/models/runner/training_runner.h"
#include <algorithm>
#include <memory>
#include <sstream>
#include <thread>

#include "core/common/logging/logging.h"
#include "core/common/logging/sinks/clog_sink.h"
#include "core/framework/tensorprotoutils.h"
#include "core/platform/env.h"
#include "core/platform/path_lib.h"
#ifdef ENABLE_NVTX_PROFILE
#include "core/profile/context.h"
#endif
#include "core/session/environment.h"
#include "orttraining/core/framework/checkpointing.h"
#include "orttraining/core/graph/optimizer_graph_builder.h"
#include "orttraining/models/runner/training_util.h"
#include "single_include/nlohmann/json.hpp"
#include "test/perftest/utils.h"

using json = nlohmann::json;

namespace onnxruntime {
namespace training {

static std::vector<FreeDimensionOverride> overrides = {};
static SessionOptions SESSION_OPTION = {
    ExecutionMode::ORT_SEQUENTIAL,     //execution_mode
    false,                             //enable_profiling
    ORT_TSTR(""),                      //optimized_model_filepath
    true,                              //enable_mem_pattern
    true,                              //enable_cpu_mem_arena
    ORT_TSTR("onnxruntime_profile_"),  //profile_file_prefix
    "",                                //session_logid
    -1,                                //session_log_severity_level
    0,                                 //session_log_verbosity_level
    5,                                 //max_num_graph_transformation_steps
    TransformerLevel::Level1,          //graph_optimization_level
    {},                                //intra_op_param
    {},                                //inter_op_param
    overrides,                         //free_dimension_overrides
    true,                              //use_per_session_threads
    true                               //thread_pool_allow_spinning
};

TrainingRunner::TrainingRunner(Parameters params, const Environment& env)
    : TrainingRunner(params, env, SESSION_OPTION) {
}

TrainingRunner::TrainingRunner(Parameters params, const Environment& env, SessionOptions session_options)
    : step_(0),
      round_(0),
      weight_update_step_count_(0),
      training_data_set_index_(0),
      params_(params),
      session_options_(session_options),
      session_(session_options, env),
      input_allocator_(params.input_allocator ? params.input_allocator : TrainingUtil::GetCpuAllocator()),
      pipeline_schedule_(params_.pipeline_parallel_size),
      pipeline_worker_pool_(params_.pipeline_parallel_size) {
  ORT_ENFORCE(!params_.model_path.empty());
  if (!params.weights_to_train.empty())
    ORT_ENFORCE(params.weights_not_to_train.empty());
  ORT_ENFORCE(!params_.training_optimizer_name.empty());
  if (params.deepspeed_zero.stage != 0)
    ORT_ENFORCE(params.use_nccl,
                "DeepSpeed ZeRO partitioning is only supported with NCCL distributed training.");
  ORT_ENFORCE(params.num_train_steps % params.gradient_accumulation_steps == 0,
              "Number of training steps must be a multiple of number of gradient accumulation step.");
}

Status TrainingRunner::Initialize() {
  if (params_.pipeline_parallel_size > 1 && !params_.pipeline_stage_paths.empty()) {
    // Pipeline partition happens outside ORT. We just load the result of partitioning forward graph.
    // Backward graph will be generated using ORT's graph transformers.
    ORT_ENFORCE(static_cast<size_t>(params_.mpi_context.world_size) == params_.pipeline_stage_paths.size());
    ORT_RETURN_IF_ERROR(session_.Load(params_.pipeline_stage_paths[params_.mpi_context.world_rank]));
  } else {
    ORT_RETURN_IF_ERROR(session_.Load(params_.model_path));
  }

  TrainingSession::TrainingConfiguration config{};
  config.model_with_loss_function_path = params_.model_with_loss_func_path;
  config.model_with_training_graph_path = params_.model_with_training_graph_path;

  config.weight_names_to_train = params_.weights_to_train;
  config.weight_names_to_not_train = params_.weights_not_to_train;
  config.immutable_weights = params_.immutable_weights;

  config.gradient_graph_config.use_invertible_layernorm_grad = params_.use_invertible_layernorm_grad;
  config.set_gradients_as_graph_outputs = false;

  config.gradient_accumulation_steps = params_.gradient_accumulation_steps;

  config.distributed_config.world_rank = params_.mpi_context.world_rank;
  config.distributed_config.world_size = params_.mpi_context.world_size;
  config.distributed_config.local_size = params_.mpi_context.local_size;
  config.distributed_config.local_rank = params_.mpi_context.local_rank;
  config.distributed_config.data_parallel_size = params_.data_parallel_size;
  config.distributed_config.horizontal_parallel_size = params_.horizontal_parallel_size;
  config.distributed_config.pipeline_parallel_size = params_.pipeline_parallel_size;

  if (params_.use_mixed_precision) {
    TrainingSession::TrainingConfiguration::MixedPrecisionConfiguration mp{};
    mp.use_fp16_initializers = params_.use_fp16_initializer;

    config.mixed_precision_config = mp;
  }

  // always configure the loss function
  if (params_.pipeline_parallel_size == 1 || params_.mpi_context.world_rank == params_.mpi_context.world_size - 1) {
    TrainingSession::TrainingConfiguration::LossFunctionConfiguration lf{};
    lf.loss_function_info = params_.loss_func_info;

    config.loss_function_config = lf;
  }

  // always configure the optimizer
  {
    TrainingSession::TrainingConfiguration::OptimizerConfiguration opt{};
    opt.name = params_.training_optimizer_name;
    opt.learning_rate_input_name = params_.lr_params.feed_name;
    opt.weight_attributes_generator = params_.optimizer_attributes;
    opt.weight_int_attributes_generator = params_.optimizer_int_attributes;
    opt.use_fp16_moments = params_.use_fp16_moments;
    opt.do_all_reduce_in_fp16 = params_.allreduce_in_fp16;
    opt.use_nccl = params_.use_nccl;
    opt.deepspeed_zero = params_.deepspeed_zero;
    opt.adasum_reduction_type = params_.GetAdasumReductionType();
    opt.enable_grad_norm_clip = params_.enable_grad_norm_clip;
    config.optimizer_config = opt;
  }

  if (params_.EnableTensorboard()) {
    TrainingSession::TrainingConfiguration::TensorboardConfiguration tb{};
    tb.summary_name = params_.summary_name;
    tb.scalar_node_names = params_.scalar_names;
    tb.histogram_node_names = params_.histogram_names;
    tb.norm_node_names = params_.norm_names;
    tb.dump_convergence_metrics = params_.dump_convergence_metrics;

    config.tensorboard_config = tb;
  }

  if (params_.use_gist) {
    TrainingSession::TrainingConfiguration::GistConfiguration gist{};

    config.gist_config = gist;
  }

  // Prepare pipeline information to do configuration.
  if (params_.pipeline_parallel_size > 1) {
    TrainingSession::TrainingConfiguration::PipelineConfiguration pipe{};
    // If partition is done outside and the paths to partitioned model are provided,
    // the session already loads a pipeline stage.
    pipe.do_partition = params_.pipeline_stage_paths.empty() ? true : false;
    pipe.fetch_names = params_.fetch_names;
    pipe.cut_list = params_.pipeline_partition_cut_list;
    // Do not assign value to config.pipeline_config if pipeline is not used.
    config.pipeline_config = pipe;
  }

  config.enable_gelu_approximation = params_.enable_gelu_approximation;

  TrainingSession::TrainingConfigurationResult config_result{};

  ORT_RETURN_IF_ERROR(session_.ConfigureForTraining(config, config_result));

  if (config_result.mixed_precision_config_result.has_value()) {
    const std::string& loss_scale_input_name =
        config_result.mixed_precision_config_result.value().loss_scale_input_name;
    if (params_.loss_scale == 0.0f) {
      // use dynamic loss_scale
      loss_scaler_ = onnxruntime::make_unique<LossScaler>(loss_scale_input_name, true, static_cast<float>(1 << 16));
    } else {
      // use static loss_scale
      loss_scaler_ = onnxruntime::make_unique<LossScaler>(loss_scale_input_name, false, params_.loss_scale);
    }
  }

  opt_graph_outputs_ = config_result.opt_config_result.value().output_key_to_graph_output_name;

  // Retrieve pipeline information from configuration result.
  VectorString fetch_names;
  if (params_.pipeline_parallel_size > 1) {
    fetch_names = config_result.pipeline_config_result.value().fetch_names;
    // Exposes forward waited event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->FW->Record->Send->Record
    //  ^
    //  |
    // this event's operator.
    pipeline_context_.forward_waited_event_name = config_result.pipeline_config_result.value().forward_waited_event_name;

    // Exposes forward waited event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->FW->Record->Send->Record
    //              ^
    //              |
    //             this event's operator.
    pipeline_context_.forward_waited_event_after_recv_name = config_result.pipeline_config_result.value().forward_waited_event_after_recv_name;

    // Exposes forward recorded event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->FW->Record->Send->Record
    //                         ^
    //                         |
    //                        this event's operator.
    pipeline_context_.forward_recorded_event_before_send_name = config_result.pipeline_config_result.value().forward_recorded_event_before_send_name;

    // Exposes forward recorded event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->FW->Record->Send->Record
    //                                       ^
    //                                       |
    //                                      this event's operator.
    pipeline_context_.forward_recorded_event_name = config_result.pipeline_config_result.value().forward_recorded_event_name;

    // Exposes backward waited event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->BW->Record->Send->Record
    //  ^
    //  |
    // this event's operator.
    pipeline_context_.backward_waited_event_name = config_result.pipeline_config_result.value().backward_waited_event_name;

    // Exposes backward waited event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->BW->Record->Send->Record
    //              ^
    //              |
    //             this event's operator.
    pipeline_context_.backward_waited_event_after_recv_name = config_result.pipeline_config_result.value().backward_waited_event_after_recv_name;

    // Exposes backward recorded event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->BW->Record->Send->Record
    //                         ^
    //                         |
    //                        this event's operator.
    pipeline_context_.backward_recorded_event_before_send_name = config_result.pipeline_config_result.value().backward_recorded_event_before_send_name;

    // Exposes backward recorded event tensor ID name to TrainingRunner.
    // It's an input of a graph.
    // Wait->Recv->Wait->BW->Record->Send->Record
    //                                       ^
    //                                       |
    //                                      this event's operator.
    pipeline_context_.backward_recorded_event_name = config_result.pipeline_config_result.value().backward_recorded_event_name;

    pipeline_context_.forward_wait_output_name = config_result.pipeline_config_result.value().forward_wait_output_name;
    pipeline_context_.forward_record_output_name = config_result.pipeline_config_result.value().forward_record_output_name;
    pipeline_context_.backward_wait_output_name = config_result.pipeline_config_result.value().backward_wait_output_name;
    pipeline_context_.backward_record_output_name = config_result.pipeline_config_result.value().backward_record_output_name;

    if (!pipeline_context_.forward_wait_output_name.empty()) {
      fetch_names.push_back(pipeline_context_.forward_wait_output_name);
    }

    if (!pipeline_context_.forward_record_output_name.empty()) {
      fetch_names.push_back(pipeline_context_.forward_record_output_name);
    }

    if (!pipeline_context_.backward_wait_output_name.empty()) {
      fetch_names.push_back(pipeline_context_.backward_wait_output_name);
    }

    if (!pipeline_context_.backward_record_output_name.empty()) {
      fetch_names.push_back(pipeline_context_.backward_record_output_name);
    }

    // Names of allowed inputs after pipeline partition.
    pipeline_context_.feed_names = config_result.pipeline_config_result.value().feed_names;
    // Names of allowed outputs after pipeline partition.
    pipeline_context_.fetch_names = config_result.pipeline_config_result.value().fetch_names;

    // Configure dimension of this pipeline.
    pipeline_context_.pipeline_stage_id = config_result.pipeline_config_result.value().pipeline_stage_id;
    pipeline_context_.num_pipeline_batches = params_.gradient_accumulation_steps;
    pipeline_schedule_.Add(0, pipeline_context_.num_pipeline_batches);
  } else {
    fetch_names = params_.fetch_names;
    pipeline_context_.pipeline_stage_id = 0;
  }

  // Expose all optimizer outputs as graph outputs.
  for (const auto& it : opt_graph_outputs_) {
    fetch_names.push_back(it.second);
  }

  // Expose all optimizer outputs and pipeline outputs and as graph outputs.
  ORT_RETURN_IF_ERROR(session_.OverrideGraphOutputs(fetch_names));

  for (const auto& factory : params_.providers) {
    auto provider = factory.second->CreateProvider();
    ORT_ENFORCE(factory.first == provider->Type());
    ORT_RETURN_IF_ERROR(session_.RegisterExecutionProvider(std::move(provider)));
  }

  if (params_.use_profiler && !session_options_.enable_profiling) {
    // Profiling has not already been enabled, so override from command line options.
    session_.StartProfiling(session_options_.profile_file_prefix);
  }

  ORT_RETURN_IF_ERROR(session_.Initialize());

  // Checkpointing initialization
  // session_.Initialize() must be called prior to LoadCheckpoint()
  if (!params_.checkpoints_dir.empty()) {
    checkpoint_registry_ = onnxruntime::make_unique<CheckpointRegistry>(
        params_.checkpoints_dir, params_.max_num_checkpoints);

    // Load checkpoint, if any
    PathString checkpoint_to_load_path = params_.checkpoint_to_load_path;
    if (!checkpoint_to_load_path.empty() ||
        checkpoint_registry_->TryGetLatestCheckpoint(checkpoint_to_load_path)) {
      ORT_RETURN_IF_ERROR(LoadCheckpoint(checkpoint_to_load_path));
    }
  }

  return Status::OK();
}

Status TrainingRunner::Run(IDataLoader* training_data_loader, IDataLoader* test_data_loader,
                           const MapStringToString& mapped_dimensions) {
  if (params_.mpi_context.world_rank == 0 && !params_.model_actual_running_graph_path.empty()) {
    session_.Save(params_.model_actual_running_graph_path, TrainingSession::SaveOption::NO_RELOAD);
  }

  // maybe in the future we can support an evaluation-only run
  if (!training_data_loader) {
    LOGS_DEFAULT(WARNING) << "training data loader not provided, nothing to do";
    return Status::OK();
  }

  ORT_RETURN_IF_ERROR(TrainingLoop(*training_data_loader, test_data_loader, mapped_dimensions));

  // after successful Run(), update counters
  ++round_;
  step_ = 0;

  return Status::OK();
}

// Prepare feeds for a call to one session run.
Status TrainingRunner::PrepareFeedNamesAndFeeds(const SessionMode mode,
                                                IDataLoader& training_data_loader,
                                                DataSet& training_data,
                                                LearningRateScheduler* lr_scheduler,
                                                const size_t batch_index,
                                                std::vector<std::string>& feed_names,
                                                std::vector<MLValue>& feeds) {
  // Initialize outputs of this function.
  feed_names = std::vector<std::string>();
  feeds = std::vector<MLValue>();

  auto allowed_feed_begin = pipeline_context_.feed_names.begin();
  auto allowed_feed_end = pipeline_context_.feed_names.end();

  // Pick up feeds from data loader
  {
    std::vector<std::string> data_feed_names = training_data_loader.DataSetTensorNames();
    std::vector<MLValue> data_feeds = training_data.GetKthBatch(params_.batch_size, batch_index, input_allocator_);
    for (size_t i = 0; i < data_feed_names.size(); ++i) {
      const auto name = data_feed_names[i];
      if (params_.pipeline_parallel_size == 1 || std::find(allowed_feed_begin, allowed_feed_end, name) != allowed_feed_end) {
        feed_names.push_back(name);
        feeds.push_back(data_feeds[i]);
      }
    }
  }

  // Pick up feed from loss scaling.
  if (loss_scaler_) {
    const auto name = loss_scaler_->GetLossScaleInputName();
    if (params_.pipeline_parallel_size == 1 || std::find(allowed_feed_begin, allowed_feed_end, name) != allowed_feed_end) {
      feed_names.push_back(name);
      const float loss_scale = (mode == EvaluateStep) ? 1.0f : loss_scaler_->GetLossScale();
      OrtValue loss_scale_val;
      TrainingUtil::CreateCpuMLValue({1}, std::vector<float>{loss_scale}, &loss_scale_val, input_allocator_);
      feeds.push_back(loss_scale_val);
    }
  }

  // Pick up feed from learning rate schedule.
  {
    const auto name = params_.lr_params.feed_name;
    if (params_.pipeline_parallel_size == 1 || std::find(allowed_feed_begin, allowed_feed_end, name) != allowed_feed_end) {
      feed_names.push_back(name);
      // learning rate is 0 if there is no learning-rate scheduler. Otherwise, learning rate is obtained from the scheduler.
      const float learning_rate = lr_scheduler ? lr_scheduler->GetLearningRate(step_ + 1) : 0.0f;
      OrtValue lr_val;
      TrainingUtil::CreateCpuMLValue({1}, std::vector<float>{learning_rate}, &lr_val, input_allocator_);
      feeds.push_back(lr_val);
    }
  }

  // Create feed of the first waited event in forward pass.
  if (!pipeline_context_.forward_waited_event_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.forward_waited_event_name);
    OrtValue event_id;
    const int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetForwardWaitedEventId(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  // Create feed of the second waited event in forward pass.
  if (!pipeline_context_.forward_waited_event_after_recv_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.forward_waited_event_after_recv_name);
    OrtValue event_id;
    const int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetForwardWaitedEventIdAfterRecv(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  // Create feed of first recorded event in forward pass.
  if (!pipeline_context_.forward_recorded_event_before_send_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.forward_recorded_event_before_send_name);
    OrtValue event_id;
    const int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetForwardRecordedEventIdBeforeSend(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  // Create feed of second recorded event in forward pass.
  if (!pipeline_context_.forward_recorded_event_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.forward_recorded_event_name);
    OrtValue event_id;
    const int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetForwardRecordedEventId(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  // Create feed of first waited event in backward pass.
  if (!pipeline_context_.backward_waited_event_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.backward_waited_event_name);
    OrtValue event_id;
    const int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetBackwardWaitedEventId(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  // Create feed of second waited event in backward pass.
  if (!pipeline_context_.backward_waited_event_after_recv_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.backward_waited_event_after_recv_name);
    OrtValue event_id;
    const int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetBackwardWaitedEventIdAfterRecv(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  // Create feed of first recorded event in backward pass.
  if (!pipeline_context_.backward_recorded_event_before_send_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.backward_recorded_event_before_send_name);
    OrtValue event_id;
    int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetBackwardRecordedEventIdBeforeSend(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  // Create feed of second recorded event in backward pass.
  if (!pipeline_context_.backward_recorded_event_name.empty()) {
    ORT_RETURN_IF(params_.pipeline_parallel_size <= 1, "Internal event name should be empty if there is no pipeline.");
    feed_names.push_back(pipeline_context_.backward_recorded_event_name);
    OrtValue event_id;
    int64_t id =
        (mode == EvaluateStep) ? -1
                               : pipeline_schedule_.GetBackwardRecordedEventId(
                                     pipeline_context_.pipeline_stage_id,
                                     static_cast<int>(step_) % pipeline_context_.num_pipeline_batches);
    TrainingUtil::CreateCpuMLScalar(
        id,
        &event_id,
        input_allocator_);
    feeds.push_back(event_id);
  }

  return Status::OK();
}

Status TrainingRunner::PrepareFetchNamesAndFetches(const SessionMode mode,
                                                   std::vector<std::string>& fetch_names,
                                                   std::vector<MLValue>& fetches) {
  // Initialize outputs of this function.
  fetch_names = std::vector<std::string>();
  fetches = std::vector<MLValue>();

  const auto& allowed_fetch_names = pipeline_context_.fetch_names;

  if (mode == ModelUpdateStep) {
    // Set up tensor to be fetched when doing model update.

    if (params_.pipeline_parallel_size > 1) {
      // If pipeline is used, we need to filter out fetches which are not in this pipeline stage.

      for (size_t i = 0; i < params_.fetch_names.size(); ++i) {
        const auto name = params_.fetch_names[i];
        auto it = std::find(allowed_fetch_names.begin(), allowed_fetch_names.end(), name);
        if (it == allowed_fetch_names.end()) {
          continue;
        }
        fetch_names.push_back(name);
      }
    } else {
      // No pipeline. All fetched names should appear in the graph handled by this process.
      fetch_names = params_.fetch_names;

      if (params_.use_mixed_precision) {
        auto it = opt_graph_outputs_.find(OptimizerOutputKey::GradientAllIsFinite);
        ORT_RETURN_IF(it == opt_graph_outputs_.end(), "Gradient norm's IsFinite output is missing in the optimizer output");
        fetch_names.push_back(it->second);
        if (params_.use_adasum) {
          it = opt_graph_outputs_.find(OptimizerOutputKey::DeltaAllIsFinite);
          ORT_RETURN_IF(it == opt_graph_outputs_.end(), "Adasum delta's IsFinite output is missing in the optimizer output");
          fetch_names.push_back(it->second);
        }
      }
    }
  } else if (mode == GradientAccumulateStep) {
    // Set up tensor to be fetched when doing gradient accumulation.

    if (params_.gradient_accumulation_steps > 1) {
      auto it = opt_graph_outputs_.find(OptimizerOutputKey::GradientAccumulation);
      ORT_RETURN_IF(it == opt_graph_outputs_.end(), "Gradient accumulation output is missing in the optimizer output");
      fetch_names.push_back(it->second);
    }

    // Always execute event operators to avoid deadlock if pipeline is used.
    // TODO: create a list of must-to-fetch tensors and pass it to all graph transformer.
    if (params_.pipeline_parallel_size) {
      if (!pipeline_context_.forward_wait_output_name.empty()) {
        fetch_names.push_back(pipeline_context_.forward_wait_output_name);
      }
      if (!pipeline_context_.forward_record_output_name.empty()) {
        fetch_names.push_back(pipeline_context_.forward_record_output_name);
      }
      if (!pipeline_context_.backward_wait_output_name.empty()) {
        fetch_names.push_back(pipeline_context_.backward_wait_output_name);
      }
      if (!pipeline_context_.backward_record_output_name.empty()) {
        fetch_names.push_back(pipeline_context_.backward_record_output_name);
      }
    }
  } else if (mode == EvaluateStep) {
    // Set up tensor to be fetched when doing model evaluation.
    // Ideally, this path should not fetch optimizer and gradient accumulation.
    // This path may fetch predicted scores, loss value, and so on.

    if (params_.pipeline_parallel_size > 1) {
      // If pipeline is used, we need to filter out fetches which are not in this pipeline stage.

      for (size_t i = 0; i < params_.fetch_names.size(); ++i) {
        const auto name = params_.fetch_names[i];
        auto it = std::find(allowed_fetch_names.begin(), allowed_fetch_names.end(), name);
        if (it == allowed_fetch_names.end()) {
          continue;
        }
        fetch_names.push_back(name);
      }
    } else {
      // No pipeline. All fetched names should appear in the graph handled by this process.
      fetch_names = params_.fetch_names;
    }
  }

  // We need to fetch at least one variable.
  // If there is nothing to fetch, we fetch all model outputs.
  if (fetch_names.empty()) {
    fetch_names = allowed_fetch_names;
  }

  return Status::OK();
}

// Launch synced session.Run on the main thread.
void TrainingRunner::RunWithUpdate(VectorString& feed_names,
                                   VectorString& fetch_names,
                                   std::vector<MLValue>& feeds,
                                   std::vector<MLValue>& fetches) {
  // Cyclically pick up a worker ID.
  const size_t worker_id = step_ % params_.pipeline_parallel_size;

  // Wait for the previous work to finish its job.
  // Its resource cannot be overrided when it's still working.
  pipeline_worker_pool_.Join(worker_id);

  // Copy thread-used variable to thread-specific buffer to maintain their life.
  pipeline_worker_pool_.worker_states[worker_id].feed_names = feed_names;
  pipeline_worker_pool_.worker_states[worker_id].feeds = feeds;
  pipeline_worker_pool_.worker_states[worker_id].fetch_names = fetch_names;
  pipeline_worker_pool_.worker_states[worker_id].fetches = std::vector<MLValue>();

  Status status = Status::OK();
  pipeline_worker_pool_.workers[worker_id] = std::thread([&](
                                                             const size_t worker_id, const size_t step) {
#ifdef ENABLE_NVTX_PROFILE
    // Store the tag for the thread which runs session_.Run(...).
    // It will be used to name range in Nvidia's visual profiler.
    auto& profile_context = profile::Context::GetInstance();
    profile_context.SetThreadTag(
        std::this_thread::get_id(), std::to_string(step));
#else
    ORT_UNUSED_PARAMETER(step);
#endif
    status = session_.Run(
        RunOptions(),
        pipeline_worker_pool_.worker_states[worker_id].feed_names,
        pipeline_worker_pool_.worker_states[worker_id].feeds,
        pipeline_worker_pool_.worker_states[worker_id].fetch_names,
        &(pipeline_worker_pool_.worker_states[worker_id].fetches));
  },
                                                         worker_id, step_);

  // Wait all workers to finish this round of pipeline parallelism.
  // The last batch in a pipeline collects gradient and update the model.
  // We must join here because main thread needs to access thread-produced
  // fetches and those fetches must be ready.
  pipeline_worker_pool_.JoinAll();

  // If the updating thread fails, we return with its error status.
  ORT_THROW_IF_ERROR(status);

  // Copy back from thread-specific buffer to main thread's memory.
  fetches = pipeline_worker_pool_.worker_states[worker_id].fetches;

  if (loss_scaler_) {
    auto it = std::find(fetch_names.begin(), fetch_names.end(), opt_graph_outputs_[OptimizerOutputKey::GradientAllIsFinite]);
    if (it != fetch_names.end()) {
      const size_t index = static_cast<size_t>(std::distance(fetch_names.begin(), it));
      const Tensor& all_is_finite_t = fetches[index].Get<Tensor>();
      const bool is_all_finite = *(all_is_finite_t.template Data<bool>());
      loss_scaler_->UpdateLossScale(is_all_finite);
    }
  }

  // Assume that only the last pipeline stage can see loss, predicted value, and so on.
  // Thus, the error function should only be called when we are at the last stage.
  const bool session_can_see_loss = params_.pipeline_parallel_size == 1 ||
                                    pipeline_context_.pipeline_stage_id == params_.pipeline_parallel_size - 1;
  if (session_can_see_loss &&
      !params_.is_perf_test &&
      weight_update_step_count_ % params_.display_loss_steps == 0) {
    if (params_.error_function) {
      params_.error_function(feed_names, feeds, fetch_names, fetches, weight_update_step_count_);
    }
    if (params_.post_evaluation_callback) {
      params_.post_evaluation_callback(params_.batch_size, weight_update_step_count_, "train");
    }
  }

  // Wait all workers to finish this around of pipeline parallism.
  // The last batch in a pipeline collects gradient and update the model.
  pipeline_worker_pool_.JoinAll();

  // Add one after process one batch.
  ++step_;
  // Add one after update the model once.
  ++weight_update_step_count_;
}

// Launch async session.Run on non-main thread.
void TrainingRunner::RunWithoutUpdate(VectorString& feed_names,
                                      VectorString& fetch_names,
                                      std::vector<MLValue>& feeds,
                                      size_t& gradient_accumulation_step_count) {
  // Cyclically pick up a worker ID.
  const size_t worker_id = step_ % params_.pipeline_parallel_size;

  // Wait for the previous work to finish its job.
  // Its resource cannot be overrided when it's still working.
  pipeline_worker_pool_.Join(worker_id);

  // Prepare async launch of session.
  // All used variables have to be copied to a buffer object to maintain their lifetime.
  pipeline_worker_pool_.worker_states[worker_id].feeds = feeds;
  pipeline_worker_pool_.worker_states[worker_id].feed_names = feed_names;
  pipeline_worker_pool_.worker_states[worker_id].fetch_names = fetch_names;
  pipeline_worker_pool_.worker_states[worker_id].fetches = std::vector<MLValue>();

  // Async launch of a session.
  pipeline_worker_pool_.workers[worker_id] = std::thread([&](
                                                             const size_t worker_id, const size_t step) {
#ifdef ENABLE_NVTX_PROFILE
    // Store the tag for the thread which runs session_.Run(...).
    // It will be used to name range in Nvidia's visual profiler.
    auto& profile_context = profile::Context::GetInstance();
    profile_context.SetThreadTag(
        std::this_thread::get_id(), std::to_string(step));
#else
    ORT_UNUSED_PARAMETER(step);
#endif
    RunOptions run_options;
    run_options.only_execute_path_to_fetches = true;
    auto status = session_.Run(
        run_options,
        pipeline_worker_pool_.worker_states[worker_id].feed_names,
        pipeline_worker_pool_.worker_states[worker_id].feeds,
        pipeline_worker_pool_.worker_states[worker_id].fetch_names,
        &(pipeline_worker_pool_.worker_states[worker_id].fetches));
    ORT_THROW_IF_ERROR(status);
  },
                                                         worker_id, step_);

  // Add one after process one batch.
  ++step_;
  // Add one after comuting one forward-backward path without applying optimizer.
  ++gradient_accumulation_step_count;
}

Status TrainingRunner::TrainingLoop(IDataLoader& training_data_loader, IDataLoader* test_data_loader,
                                    const MapStringToString& mapped_dimensions) {
  const bool enable_checkpoint_saving =
      params_.mpi_context.world_rank == 0 &&
      checkpoint_registry_ && params_.checkpoint_period > 0;

  std::unique_ptr<perftest::utils::ICPUUsage> cpu_usage_calculator;
  if (!params_.perf_output_dir.empty()) {
    cpu_usage_calculator = perftest::utils::CreateICPUUsage();
  }

  if (test_data_loader) {
    ORT_RETURN_IF_ERROR(test_data_loader->InitializeDataSetIndex(0));
  }
  ORT_RETURN_IF_ERROR(training_data_loader.InitializeDataSetIndex(training_data_set_index_));

  const size_t num_shards_to_visit = training_data_loader.NumShards();
  const auto lr_scheduler = LearningRateScheduler::Create(params_.lr_params, params_.num_train_steps);

  double total_time{0};
  size_t epoch = 0;  // Note: epoch is not set properly when loaded from a checkpoint, but it's only for display.
  size_t gradient_accumulation_step_count = 0;
  const auto step_start = step_;
  const auto weight_update_step_count_start = weight_update_step_count_;

  // how many steps at last we used for stabilized perf benchmarking.
  const size_t stabilized_perf_total_step_count = std::min(static_cast<size_t>(128), params_.num_train_steps);
  const size_t stabilized_perf_start_step = params_.num_train_steps - stabilized_perf_total_step_count;
  double stabilized_total_time{0};
  const size_t end_to_end_perf_start_step = 128;
  auto end_to_end_start = std::chrono::high_resolution_clock::now();
  bool end_to_end_measurement_started = false;

  auto all_steps_time_start = std::chrono::high_resolution_clock::now();
  while (step_ < params_.num_train_steps) {
    for (size_t shard_it = 0; shard_it < num_shards_to_visit; ++shard_it) {
      auto training_data = training_data_loader.CurrentDataSet();
      training_data_set_index_ = training_data_loader.CurrentDataSetIndex();
      if (training_data == nullptr) {
        printf("Skipping shard at index %d, which failed to load.\n",
               static_cast<int>(training_data_loader.CurrentDataSetIndex()));
        training_data_loader.MoveToNextDataSet();
        continue;
      }

      // Shuffle the data for each epoch
      if (params_.shuffle_data) {
        printf("Randomly shuffle training data.\n");
        training_data->RandomShuffle();
      }

      // loop through the data
      size_t batch_num_cur_shard = training_data->TotalBatch(params_.batch_size);
      for (size_t batch = 0; batch < batch_num_cur_shard && step_ < params_.num_train_steps; ++batch) {
        const bool is_weight_update_step = (step_ + 1) % params_.gradient_accumulation_steps == 0;

        const bool stablized_perf_measurement_started = step_ >= stabilized_perf_start_step;
        if (!end_to_end_measurement_started && step_ >= end_to_end_perf_start_step) {
          end_to_end_start = std::chrono::high_resolution_clock::now();
          end_to_end_measurement_started = true;
        }

        VectorString feed_names;
        VectorString fetch_names;
        std::vector<MLValue> feeds;
        std::vector<MLValue> fetches;

        auto start = std::chrono::high_resolution_clock::now();

        if (is_weight_update_step) {
          ORT_RETURN_IF_ERROR(PrepareFeedNamesAndFeeds(ModelUpdateStep,
                                                       training_data_loader,
                                                       *training_data,
                                                       lr_scheduler.get(),
                                                       batch,
                                                       feed_names,
                                                       feeds));
          ORT_RETURN_IF_ERROR(
              PrepareFetchNamesAndFetches(ModelUpdateStep,
                                          fetch_names,
                                          fetches));
          RunWithUpdate(feed_names, fetch_names, feeds, fetches);
        } else {
          ORT_RETURN_IF_ERROR(PrepareFeedNamesAndFeeds(GradientAccumulateStep,
                                                       training_data_loader,
                                                       *training_data,
                                                       lr_scheduler.get(),
                                                       batch,
                                                       feed_names,
                                                       feeds));
          ORT_RETURN_IF_ERROR(
              PrepareFetchNamesAndFetches(GradientAccumulateStep,
                                          fetch_names,
                                          fetches));
          RunWithoutUpdate(feed_names, fetch_names, feeds,
                           gradient_accumulation_step_count);
        }

        // at this point, step_ already be increased by 1.
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duration_seconds = end - start;
        total_time += duration_seconds.count();
        if (stablized_perf_measurement_started) {
          stabilized_total_time += duration_seconds.count();
        }

        printf("Stage %d, Round %d, Step: %d, epoch: %d, batch: %d/%d, shard_iteration: %d/%d, time: %.2f ms, throughput: %.2f ex/sec \n",
               pipeline_context_.pipeline_stage_id,
               static_cast<int>(round_),
               static_cast<int>(step_),
               static_cast<int>(epoch),
               static_cast<int>(batch),
               static_cast<int>(batch_num_cur_shard),
               static_cast<int>(shard_it + 1),
               static_cast<int>(num_shards_to_visit),
               duration_seconds.count() * 1000,
               params_.batch_size * (step_ - step_start) / total_time);
        printf("Training data range: [%d - %d)\n",
               static_cast<int>(batch * params_.batch_size),
               static_cast<int>((batch + 1) * params_.batch_size - 1));

        if (test_data_loader &&
            params_.do_eval && step_ % params_.evaluation_period == 0) {
          ORT_RETURN_IF_ERROR(Evaluate(session_, *test_data_loader));
        }

        if (enable_checkpoint_saving && is_weight_update_step &&
            weight_update_step_count_ % params_.checkpoint_period == 0) {
          PathString new_checkpoint_path, old_checkpoint_path;
          bool should_remove_old_checkpoint;

          ORT_RETURN_IF_ERROR(checkpoint_registry_->AddCheckpoint(
              weight_update_step_count_, new_checkpoint_path,
              should_remove_old_checkpoint, old_checkpoint_path));

          // ensure checkpoint directory exists
          if (!Env::Default().FolderExists(params_.checkpoints_dir)) {
            ORT_RETURN_IF_ERROR(Env::Default().CreateFolder(params_.checkpoints_dir));
          }

          if (should_remove_old_checkpoint) {
            const auto status = Env::Default().DeleteFolder(old_checkpoint_path);
            LOGS_DEFAULT_IF(!status.IsOK(), WARNING)
                << "Failed to delete old checkpoint. "
                << "Path: " << ToMBString(old_checkpoint_path)
                << ", error: " << status.ErrorMessage();
          }

          ORT_RETURN_IF_ERROR(SaveCheckpoint(new_checkpoint_path));
        }
      }  // end of one file/shard

      pipeline_worker_pool_.JoinAll();
      if (step_ < params_.num_train_steps) {
        training_data_loader.MoveToNextDataSet();
      }
    }  // end of one epoch

    ++epoch;
  }
  auto all_steps_time_end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> all_steps_duration_seconds = all_steps_time_end - all_steps_time_start;

  const double e2e_throughput = [&]() {
    if (end_to_end_perf_start_step >= params_.num_train_steps) return 0.0;
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration_seconds = end - end_to_end_start;
    const double total_e2e_time = duration_seconds.count();
    const size_t end_to_end_step_count = params_.num_train_steps - std::max(step_start, end_to_end_perf_start_step);
    return params_.batch_size * end_to_end_step_count / total_e2e_time;
  }();

  const size_t number_of_batches = step_ - step_start;
  const size_t weight_update_steps = weight_update_step_count_ - weight_update_step_count_start;
  const double avg_time_per_batch = total_time / (step_ - step_start) * 1000;
  const double throughput = params_.batch_size * (step_ - step_start) / total_time;
  const double stabilized_throughput = params_.batch_size / (stabilized_total_time / stabilized_perf_total_step_count);

  if (params_.perf_output_dir.empty()) {
    printf("No perf output directory specified, skipping save of trained perf metrics.\n");
  } else {
    const short average_cpu_usage = cpu_usage_calculator->GetUsage();
    const size_t peak_workingset_size = perftest::utils::GetPeakWorkingSetSize();
    ORT_RETURN_IF_ERROR(Env::Default().CreateFolder(params_.perf_output_dir));
    // saving json file
    ORT_RETURN_IF_ERROR(SavePerfMetrics(number_of_batches, gradient_accumulation_step_count, weight_update_steps,
                                        total_time, avg_time_per_batch, throughput, stabilized_throughput,
                                        e2e_throughput, mapped_dimensions,
                                        average_cpu_usage, peak_workingset_size));
  }

  std::cout << "Round: " << round_ << "\n"
            << "Batch size: " << params_.batch_size << "\n"
            << "Number of Batches: " << number_of_batches << "\n"
            << "Gradient Accumulation Steps: " << gradient_accumulation_step_count << "\n"
            << "Weight Update Steps: " << weight_update_steps << "\n"
            << "Total Running Time: " << total_time << " Seconds \n"
            << "Average Running Time Per Batch: " << avg_time_per_batch << " ms\n"
            << "Throughput: " << throughput << " Examples / Second\n"
            << "Stabilized Throughput: " << stabilized_throughput << " Examples / Second\n"
            << "EndToEnd Throughput: " << e2e_throughput << " Examples / Second\n"
            << "Average Step Time: " << all_steps_duration_seconds.count() / (step_ - step_start) << " Second\n"
            << "Average Step Throughput: " << params_.batch_size * (step_ - step_start) / (all_steps_duration_seconds.count()) << " Examples / Second\n";

  return Status::OK();
}

Status TrainingRunner::SavePerfMetrics(const size_t number_of_batches, const size_t gradient_accumulation_steps,
                                       const size_t weight_update_steps, const double total_time,
                                       const double avg_time_per_batch, const double throughput, const double stabilized_throughput,
                                       const double e2e_throughput, const MapStringToString& mapped_dimensions,
                                       const short average_cpu_usage, const size_t peak_workingset_size) {
  // populate metrics for reporting
  json perf_metrics;
  perf_metrics["Model"] = params_.model_type;

  // loop thru the mapped_dimensions and put it in json sub-structure
  std::string seq_len;
  for (auto const& it : mapped_dimensions) {
    if (it.first == "SeqLen") {
      seq_len = it.second;
    }
    perf_metrics["DerivedProperties"][it.first] = it.second;
  }

  perf_metrics["Round"] = round_;
  perf_metrics["BatchSize"] = params_.batch_size;
  perf_metrics["NumOfBatches"] = number_of_batches;
  perf_metrics["GradAccSteps"] = gradient_accumulation_steps;
  perf_metrics["WeightUpdateSteps"] = weight_update_steps;
  perf_metrics["TotalTime"] = total_time;
  perf_metrics["AvgTimePerBatch"] = avg_time_per_batch;
  perf_metrics["Throughput"] = throughput;
  perf_metrics["StabilizedThroughput"] = stabilized_throughput;
  perf_metrics["EndToEndThroughput"] = e2e_throughput;
  perf_metrics["UseMixedPrecision"] = params_.use_mixed_precision;

  std::string optimizer = params_.training_optimizer_name;
  std::size_t pos = optimizer.find("Optimizer");
  if (pos != std::string::npos)
    optimizer = optimizer.substr(0, pos);
  perf_metrics["Optimizer"] = optimizer;

  Path model_path{};
  ORT_RETURN_IF_ERROR(Path::Parse(params_.model_path, model_path));
  PathString leaf = model_path.GetComponents().back();
  std::string model_name = ToMBString(leaf.c_str());
  perf_metrics["ModelName"] = model_name;

  std::string display_name = model_name + "_" + params_.model_type + "_" + (params_.use_mixed_precision ? "fp16" : "fp32") +
                             (seq_len.empty() ? "" : "_" + seq_len) + "_" + optimizer;
  perf_metrics["DisplayName"] = display_name;

  perf_metrics["Memory"] = peak_workingset_size >> 20;  // mb
  perf_metrics["AvgCPU"] = average_cpu_usage;

  //
  // we will get date/time and commitId in post-run pipeline
  //

  // populate other basic params for bookkeeping - add more as needed
  json bookkeeping_params;
  bookkeeping_params["LearningRate"] = params_.lr_params.initial_lr;
  bookkeeping_params["WarmupRatio"] = params_.lr_params.warmup_ratio;
  bookkeeping_params["WarmupMode"] = params_.lr_params.warmup_mode;
  bookkeeping_params["TrainSteps"] = params_.num_train_steps;
  bookkeeping_params["ModelPath"] = ToMBString(params_.model_path.c_str());
  bookkeeping_params["TrainDataDir"] = ToMBString(params_.train_data_dir.c_str());
  bookkeeping_params["TestDataDir"] = ToMBString(params_.test_data_dir.c_str());

  perf_metrics["RunConfig"] = bookkeeping_params.dump();  // serialize the params as json string

  std::string json_string = perf_metrics.dump();

  // write to a file - the next task in CI will pick up all files with the same prefix
  const PathString perf_metrics_path =
      params_.perf_output_dir + GetPathSep<PathChar>() + ORT_TSTR("onnxruntime_perf_metrics_") +
      ToPathString(display_name) + ORT_TSTR(".json");

  std::ofstream perf_metrics_stream;
  perf_metrics_stream.open(perf_metrics_path, std::ios::out | std::ios::trunc);
  ORT_RETURN_IF_NOT(perf_metrics_stream << json_string << "\n", "Failed to write to output file.");

  std::cout << "\n\nSaved perf metrics file: " << ToMBString(perf_metrics_path) << "\n\n";

  return Status::OK();
}

Status TrainingRunner::EndTraining(IDataLoader* data_loader) {
  if (params_.use_profiler) {
    // Write profiler data to disk.
    // We do this first in case there are any problems saving the trained model.
    std::string profile_file = session_.EndProfiling();
    std::cout << "Profiler data written to file " << profile_file << "\n";
  }

  if (data_loader) {
    // Test the in-memory model before saving.
    printf("\nEvaluating the final model on the test set.\n");
    ORT_RETURN_IF_ERROR(Evaluate(session_, *data_loader));
  }

  if (params_.output_dir.empty()) {
    printf("No output directory specified, skipping save of trained model.\n");
    return Status::OK();
  }

  // Create output directory if needed.
  if (!params_.output_dir.empty()) {
    ORT_RETURN_IF_ERROR(Env::Default().CreateFolder(params_.output_dir));
  }

  printf("\nSaving the trained model.\n");
  const PathString model_base_name = GetLastComponent(params_.model_path);

  const PathString trained_model_path =
      params_.output_dir + GetPathSep<PathChar>() + model_base_name + ORT_TSTR("_trained.onnx");
  ORT_RETURN_IF_ERROR(session_.Save(
      trained_model_path, TrainingSession::SaveOption::WITH_UPDATED_WEIGHTS));

  const PathString trained_model_with_loss_func_path =
      params_.output_dir + GetPathSep<PathChar>() + model_base_name + ORT_TSTR("_with_cost_trained.onnx");
  ORT_RETURN_IF_ERROR(session_.Save(
      trained_model_with_loss_func_path, TrainingSession::SaveOption::WITH_UPDATED_WEIGHTS_AND_LOSS_FUNC));

  return Status::OK();
}

Status TrainingRunner::Evaluate(InferenceSession& session, IDataLoader& data_loader) {
  if (params_.skip_evaluation) {
    printf("Skipping evaluation...\n");
    return Status::OK();
  }

  // A static batch index representing current test batch
  static size_t current_batch = 0;
  auto test_data = data_loader.CurrentDataSet();
  if (params_.shuffle_data && current_batch == 0) {
    printf("Randomly shuffle test data.\n");
    test_data->RandomShuffle();
  }

  const size_t evaluation_batch_size = params_.eval_batch_size;

  printf("Test data range: [%d - %d)\n",
         static_cast<int>(current_batch * evaluation_batch_size),
         static_cast<int>((current_batch + 1) * evaluation_batch_size - 1));

  const size_t num_batches = size_t(ceil((float)evaluation_batch_size / (float)params_.batch_size));
  if (evaluation_batch_size % params_.batch_size != 0) {
    printf(
        "WARNING: evaluation_batch_size %zu is not an integer multiple of batch_size %zu. "
        "Using evaluation_batch_size %zu\n",
        evaluation_batch_size,
        params_.batch_size,
        num_batches * params_.batch_size);
  }

  RunOptions run_options;
  for (size_t batch_idx = 0; batch_idx < num_batches; ++batch_idx) {
    std::vector<std::string> feed_names;
    std::vector<MLValue> feeds;
    std::vector<std::string> fetch_names;
    std::vector<MLValue> fetches;

    PrepareFeedNamesAndFeeds(EvaluateStep,
                             data_loader,
                             *test_data,
                             nullptr,
                             batch_idx,
                             feed_names,
                             feeds);

    PrepareFetchNamesAndFetches(EvaluateStep,
                                fetch_names,
                                fetches);

    if (params_.pipeline_parallel_size == 1) {
      auto status = Status::OK();
      // When there is no pipeline, we always use the first thread
      // to launch session_.Run(...) to avoid multiple activation allocations.

      // Always use the first thread to evaluate.
      const size_t worker_id = 0;
      // Wait for the previous work to finish its job.
      // Its resource cannot be overrided when it's still working.
      pipeline_worker_pool_.Join(worker_id);
      // Declare Run(...)'s status in thread.
      // Launch Run(...).
      pipeline_worker_pool_.workers[worker_id] = std::thread([&]() {
        RunOptions run_options;
        run_options.only_execute_path_to_fetches = true;
        status = session.Run(
            run_options,
            feed_names,
            feeds,
            fetch_names,
            &fetches);
      });
      // Wait Run(...) to finish.
      pipeline_worker_pool_.Join(worker_id);
      ORT_RETURN_IF_ERROR(status);
    } else {
      // Training threads are fully used by pipeline stages.
      // Pipeline cannot reuse training threads to do evaluation.
      // Otherwise, deadlock may happens.
      ORT_RETURN_IF_ERROR(session.Run(run_options,
                                      feed_names,
                                      feeds,
                                      fetch_names,
                                      &fetches));
    }

    // Assume that user-specified fetches are avaliable only on the last pipeline stage.
    // When there is no pipeline, all pipeline_context_.pipeline_stage_id should be 0 and
    // params_.pipeline_parallel_size is 1. Thus, the following condition is always true if there
    // is no pipeline.
    const bool session_can_see_loss = pipeline_context_.pipeline_stage_id == params_.pipeline_parallel_size - 1;

    // Call error function
    if (session_can_see_loss && params_.error_function) {
      params_.error_function(feed_names, feeds, params_.fetch_names, fetches, step_);
    }

    // Set to next batch
    if (++current_batch >= test_data->TotalBatch(params_.batch_size)) {
      // Move to next shard
      test_data = data_loader.MoveToNextDataSet();
      current_batch = 0;
    }
  }

  // Call after a test batch.
  if (params_.post_evaluation_callback) {
    params_.post_evaluation_callback(evaluation_batch_size, step_, "test");
  }

  return Status::OK();
}

Status TrainingRunner::SaveCheckpoint(const PathString& checkpoint_path) {
  NameMLValMap checkpointed_tensors{};
  ORT_RETURN_IF_ERROR(session_.GetStateTensors(checkpointed_tensors));

  std::unordered_map<std::string, std::string> checkpointed_properties{};
  ORT_RETURN_IF_ERROR(SaveCheckpointProperties(checkpointed_properties));

  ORT_RETURN_IF_ERROR(SaveModelCheckpoint(
      checkpoint_path, session_.GetDataTransferManager(),
      checkpointed_tensors, checkpointed_properties));

  return Status::OK();
}

namespace {
Status WithOrtValuesFromTensorProtos(
    const PathString& model_location,
    const std::vector<ONNX_NAMESPACE::TensorProto>& tensor_protos,
    std::function<Status(const NameMLValMap&)> use_name_to_ort_value_fn) {
  static const OrtMemoryInfo cpu_alloc_info{onnxruntime::CPU, OrtDeviceAllocator};

  NameMLValMap name_to_ort_value{};
  std::vector<std::vector<char>> tensor_buffers{};
  std::vector<ScopedOrtCallbackInvoker> tensor_deleters{};

  for (const auto& tensor_proto : tensor_protos) {
    const auto* tensor_type = DataTypeImpl::TensorTypeFromONNXEnum(tensor_proto.data_type());
    const size_t element_size = tensor_type->GetElementType()->Size();
    const TensorShape shape{
        tensor_proto.dims().data(), static_cast<size_t>(tensor_proto.dims().size())};

    std::vector<char> tensor_buffer{};
    tensor_buffer.resize(element_size * shape.Size());

    const MemBuffer mem_buffer{tensor_buffer.data(), tensor_buffer.size(), cpu_alloc_info};

    OrtValue ort_value;
    OrtCallback callback;

    ORT_RETURN_IF_ERROR(utils::TensorProtoToMLValue(
        Env::Default(), model_location.c_str(), tensor_proto, mem_buffer,
        ort_value, callback));
    ScopedOrtCallbackInvoker callback_invoker{callback};

    name_to_ort_value.emplace(tensor_proto.name(), ort_value);
    tensor_buffers.emplace_back(std::move(tensor_buffer));
    tensor_deleters.emplace_back(std::move(callback_invoker));
  }

  ORT_RETURN_IF_ERROR(use_name_to_ort_value_fn(name_to_ort_value));

  return Status::OK();
}
}  // namespace

Status TrainingRunner::LoadCheckpoint(const PathString& checkpoint_path) {
  std::vector<ONNX_NAMESPACE::TensorProto> checkpointed_tensors{};
  std::unordered_map<std::string, std::string> checkpointed_properties{};
  ORT_RETURN_IF_ERROR(LoadModelCheckpoint(
      checkpoint_path, session_.GetModelLocation(),
      checkpointed_tensors, checkpointed_properties));

  ORT_RETURN_IF_ERROR(WithOrtValuesFromTensorProtos(
      session_.GetModelLocation(), checkpointed_tensors,
      [this](const NameMLValMap& name_to_ort_value) -> Status {
        ORT_RETURN_IF_ERROR(session_.SetStateTensors(name_to_ort_value, true));
        return Status::OK();
      }));

  ORT_RETURN_IF_ERROR(LoadCheckpointProperties(checkpointed_properties));

  return Status::OK();
}

namespace {
namespace property_names {
constexpr const char* k_step = "step";
constexpr const char* k_round = "round";
constexpr const char* k_weight_update_step = "weight_update_step";
constexpr const char* k_training_data_set_index = "training_data_set_index";
constexpr const char* k_loss_scaler_state = "loss_scaler_state";
}  // namespace property_names

template <typename T>
Status FromString(const std::string& s, T& t) {
  std::istringstream i{s};
  ORT_RETURN_IF_NOT(i >> t && i.eof());
  return Status::OK();
}
}  // namespace

Status TrainingRunner::SaveCheckpointProperties(
    std::unordered_map<std::string, std::string>& properties) const {
  auto save_property = [&properties](const char* name, auto val) {
    properties[name] = std::to_string(val);
  };

  save_property(property_names::k_step, step_);
  save_property(property_names::k_round, round_);
  save_property(property_names::k_weight_update_step, weight_update_step_count_);
  save_property(property_names::k_training_data_set_index, training_data_set_index_);

  if (loss_scaler_) {
    properties[property_names::k_loss_scaler_state] = loss_scaler_->SaveToString();
  }

  return Status::OK();
}

Status TrainingRunner::LoadCheckpointProperties(
    const std::unordered_map<std::string, std::string>& properties) {
  auto load_property = [&properties](const char* name, auto& val) {
    auto prop_it = properties.find(name);
    ORT_RETURN_IF_NOT(prop_it != properties.end());
    ORT_RETURN_IF_ERROR(FromString(prop_it->second, val));
    return Status::OK();
  };

  ORT_RETURN_IF_ERROR(load_property(property_names::k_step, step_));
  ORT_RETURN_IF_ERROR(load_property(property_names::k_round, round_));
  ORT_RETURN_IF_ERROR(load_property(
      property_names::k_weight_update_step, weight_update_step_count_));
  ORT_RETURN_IF_ERROR(load_property(
      property_names::k_training_data_set_index, training_data_set_index_));

  if (loss_scaler_) {
    auto prop_it = properties.find(property_names::k_loss_scaler_state);
    ORT_RETURN_IF_NOT(prop_it != properties.end());
    ORT_RETURN_IF_ERROR(loss_scaler_->LoadFromString(prop_it->second));
  }

  return Status::OK();
}

Status TrainingRunner::UpdateParams(Parameters params) {
  params_.lr_params.initial_lr = params.lr_params.initial_lr;
  params_.lr_params.warmup_ratio = params.lr_params.warmup_ratio;
  params_.num_train_steps = params.num_train_steps;
  params_.batch_size = params.batch_size;
  params_.gradient_accumulation_steps = params.gradient_accumulation_steps;
  return Status::OK();
}

Status TrainingRunner::ResetLossScaler() {
  if (loss_scaler_) {
    loss_scaler_->Reset();
  }
  return Status::OK();
}
}  // namespace training
}  // namespace onnxruntime
