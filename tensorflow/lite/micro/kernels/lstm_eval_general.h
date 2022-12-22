/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_LITE_MICRO_KERNELS_LSTM_EVAL_GENERAL_H_
#define TENSORFLOW_LITE_MICRO_KERNELS_LSTM_EVAL_GENERAL_H_
#include <algorithm>
#include <cstdint>

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/portable_tensor_utils.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/fully_connected.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/logistic.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/mul.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/tanh.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/lstm_shared.h"
#include "tensorflow/lite/micro/micro_log.h"

namespace tflite {
// Since LSTM includes multiple intermediate stages, introducing the internal
// namespace to expose them for testing
namespace lstm_internal {

class LstmStepManager {
 public:
  LstmStepManager() = delete;
  explicit LstmStepManager(const LstmSizeInfo& size_info)
      : size_info_(size_info) {}

  void UpdateTime();
  void UpdateBatch();
  void ResetTime() { current_time_ = 0; }
  const RuntimeShape InputShape() const;
  const RuntimeShape StateShape() const;

  const int InputOffset() const { return input_offset_; }
  const int OutputOffset() const { return output_offset_; }
  const int HiddenStateOffset() const { return hidden_state_offset_; }
  const int CellStateOffset() const { return cell_state_offset_; }

 private:
  int current_time_ = 0;
  int current_batch_ = 0;
  int input_offset_ = 0;
  int output_offset_ = 0;
  int hidden_state_offset_ = 0;
  int cell_state_offset_ = 0;
  // Size info is from the opdata, which reside in the persistent memory
  // (guarante to outlast LSTMStepManager, which reside in stack)
  const LstmSizeInfo& size_info_;
};

// Calculates a single LSTM gate.
// Implements the following formula:
//   gate = activate(FC(input) + FC(recurrent))
// Activation is sigmoid except for the "cell" gate (configurable, usually
// tanh)
template <typename ActivationType, typename WeightType, typename CellType,
          typename BiasType>
void CalculateLstmGateInteger(
    const LstmStepManager& step_info, const GateParameters& gate_params,
    // Input FC
    const TfLiteEvalTensor* input, const TfLiteEvalTensor* input_weight,
    const TfLiteEvalTensor* input_bias,
    // Recurrent FC
    const TfLiteEvalTensor* recurrent, const TfLiteEvalTensor* recurrent_weight,
    const TfLiteEvalTensor* recurrent_bias,
    // Output
    CellType* gate_output,
    // Scratch arrays
    CellType* fc_output_buffer, const TfLiteFusedActivation activation) {
  const auto gate_output_shape = step_info.StateShape();
  // Input FC
  tflite::reference_integer_ops::FullyConnectedGeneral<
      ActivationType, CellType, WeightType, BiasType, int64_t>(
      gate_params.input_fc_params, step_info.InputShape(),
      tflite::micro::GetTensorData<ActivationType>(input) +
          step_info.InputOffset(),
      micro::GetTensorShape(input_weight),
      tflite::micro::GetTensorData<WeightType>(input_weight),
      tflite::micro::GetTensorShape(input_bias),
      tflite::micro::GetOptionalTensorData<BiasType>(input_bias),
      gate_output_shape, gate_output);

  // Recurrent FC
  tflite::reference_integer_ops::FullyConnectedGeneral<
      ActivationType, CellType, WeightType, BiasType, int32_t>(
      gate_params.recurrent_fc_params, step_info.StateShape(),
      tflite::micro::GetTensorData<ActivationType>(recurrent) +
          step_info.HiddenStateOffset(),
      tflite::micro::GetTensorShape(recurrent_weight),
      tflite::micro::GetTensorData<WeightType>(recurrent_weight),
      tflite::micro::GetTensorShape(recurrent_bias),
      tflite::micro::GetOptionalTensorData<BiasType>(recurrent_bias),
      gate_output_shape, fc_output_buffer);

  tflite::tensor_utils::CwiseAdd(gate_output, fc_output_buffer,
                                 /*n_batch=*/gate_output_shape.DimsData()[0],
                                 /*n_state=*/gate_output_shape.DimsData()[1],
                                 gate_output);

  // Apply activation
  switch (activation) {
    case kTfLiteActSigmoid:
      reference_integer_ops::Logistic(
          0 /*data->input_multiplier*/, 0 /*data->input_left_shift */,
          gate_output_shape.FlatSize() /*NumElements(input->dims)*/,
          gate_output /* tflite::micro::GetTensorData<int16_t>(input) */,
          gate_output /*tflite::micro::GetTensorData<int16_t>(output) */);

      break;
    case kTfLiteActTanh: {
      reference_integer_ops::Tanh(0, 0, gate_output_shape, gate_output,
                                  gate_output_shape, gate_output);
    } break;
    default:
      // Only Sigmoid or Tanh is used.
      TFLITE_ASSERT_FALSE;
  }
}

template <typename CellType>
void UpdateLstmCellInteger(const LstmStepManager& step_info,
                           TfLiteEvalTensor* cell_state,
                           // Gate outputs
                           CellType* forget_gate_output,
                           const CellType* input_gate_output,
                           const CellType* cell_gate_output,
                           // Mul parameters
                           const ArithmeticParams& forget_cell_mul_params,
                           const ArithmeticParams& input_mul_params,
                           CellType* buffer, CellType clip) {
  auto cell_state_shape = step_info.StateShape();
  // Forget Gate x Cell State
  tflite::reference_integer_ops::MulElementwise(
      cell_state_shape.FlatSize(), forget_cell_mul_params, forget_gate_output,
      tflite::micro::GetTensorData<CellType>(cell_state) +
          step_info.CellStateOffset(),
      tflite::micro::GetTensorData<CellType>(cell_state) +
          step_info.CellStateOffset());

  // Input Gate x Cell Gate
  tflite::reference_integer_ops::MulElementwise(
      cell_state_shape.FlatSize(), input_mul_params, input_gate_output,
      cell_gate_output, buffer);

  // Update the cell state
  tflite::tensor_utils::CwiseAdd(
      tflite::micro::GetTensorData<CellType>(cell_state) +
          step_info.CellStateOffset(),
      buffer,
      /*n_batch=*/cell_state_shape.DimsData()[0],
      /*n_state=*/cell_state_shape.DimsData()[1],
      tflite::micro::GetTensorData<CellType>(cell_state) +
          step_info.CellStateOffset());

  if (clip > 0) {
    tflite::tensor_utils::CwiseClipping(
        tflite::micro::GetTensorData<CellType>(cell_state) +
            step_info.CellStateOffset(),
        cell_state_shape.FlatSize(), clip);
  }
}

template <typename CellType, typename ActivationType>
void UpdateLstmHiddenInteger(const LstmStepManager& step_info,
                             TfLiteEvalTensor* cell_state,
                             TfLiteEvalTensor* hidden_state,
                             const CellType* output_gate_output,
                             const ArithmeticParams& mul_params,
                             int32_t cell_state_scale_power, CellType* buffer) {
  auto cell_state_shape = step_info.StateShape();
  CellType* cell_state_data =
      tflite::micro::GetTensorData<CellType>(cell_state) +
      step_info.CellStateOffset();
  // Tanh(cell_state)
  {
    int32_t tanh_input_left_shift = (15 + cell_state_scale_power) - 3;
    if (tanh_input_left_shift < 0) /* handling negative shift value */
    {
      int32_t i;
      tanh_input_left_shift = -tanh_input_left_shift;
      for (i = 0; i < cell_state_shape.FlatSize(); i++) {
        cell_state_data[i] = cell_state_data[i] >> tanh_input_left_shift;
      }
      tanh_input_left_shift = 0;
    }
    reference_integer_ops::Tanh(0, tanh_input_left_shift, cell_state_shape,
                                cell_state_data, cell_state_shape, buffer);
  }
  // Update the hidden state
  tflite::reference_integer_ops::MulElementwiseGeneral(
      cell_state_shape.FlatSize(), mul_params, buffer, output_gate_output,
      tflite::micro::GetTensorData<ActivationType>(hidden_state) +
          step_info.HiddenStateOffset());
}

template <typename ActivationType, typename WeightType, typename CellType,
          typename BiasType>
void LstmStepInteger(const LstmStepManager& step_info,
                     const OpDataLSTM<CellType>& op_data,
                     LSTMKernelContents<CellType>& kernel_content) {
  /*Step1: Calculate gate outputs to prepare cell state update*/
  CellType* gate_internal_buffer = kernel_content.buffer3;
  CellType* forget_gate_output = kernel_content.buffer0;
  CalculateLstmGateInteger<ActivationType, WeightType, CellType, BiasType>(
      step_info, op_data.forget_gate_parameters,
      // Input FC
      kernel_content.GetInternalTensor(tflite::kLstmInputTensor),
      kernel_content.GetInternalTensor(tflite::kLstmInputToForgetWeightsTensor),
      kernel_content.GetInternalTensor(tflite::kLstmForgetGateBiasTensor),
      // Recurrent FC
      kernel_content.HiddenStateTensor(),
      kernel_content.GetInternalTensor(
          tflite::kLstmRecurrentToForgetWeightsTensor),
      /*recurrent_bias*/ nullptr,
      // Output
      forget_gate_output,
      // Scratch arrays
      gate_internal_buffer, kTfLiteActSigmoid);

  // Input Gate calculation;
  CellType* input_gate_output = kernel_content.buffer1;
  CalculateLstmGateInteger<ActivationType, WeightType, CellType, BiasType>(
      step_info, op_data.input_gate_parameters,
      // Input FC
      kernel_content.GetInternalTensor(tflite::kLstmInputTensor),
      kernel_content.GetInternalTensor(tflite::kLstmInputToInputWeightsTensor),
      kernel_content.GetInternalTensor(tflite::kLstmInputGateBiasTensor),
      // Recurrent FC
      kernel_content.HiddenStateTensor(),
      kernel_content.GetInternalTensor(
          tflite::kLstmRecurrentToInputWeightsTensor),
      /*recurrent_bias*/ nullptr,
      // Output
      input_gate_output,
      // Scratch arrays
      gate_internal_buffer, kTfLiteActSigmoid);

  // Cell Gate calculation
  CellType* cell_gate_output = kernel_content.buffer2;
  CalculateLstmGateInteger<ActivationType, WeightType, CellType, BiasType>(
      step_info, op_data.cell_gate_parameters,
      // Input FC
      kernel_content.GetInternalTensor(tflite::kLstmInputTensor),
      kernel_content.GetInternalTensor(tflite::kLstmInputToCellWeightsTensor),
      kernel_content.GetInternalTensor(tflite::kLstmCellGateBiasTensor),
      // Recurrent FC
      kernel_content.HiddenStateTensor(),
      kernel_content.GetInternalTensor(
          tflite::kLstmRecurrentToCellWeightsTensor),
      /*recurrent_bias*/ nullptr,
      // Output
      cell_gate_output,
      // Scratch arrays
      gate_internal_buffer, op_data.cell_gate_nonlinear_type);

  /*Step2: update the cell state */
  const InterGateParameters& inter_gate_params = op_data.inter_gate_parameters;
  CellType* updated_input_buffer = kernel_content.buffer1;  // reuse buffer

  UpdateLstmCellInteger<CellType>(
      step_info, kernel_content.CellStateTensor(), forget_gate_output,
      input_gate_output, cell_gate_output,
      inter_gate_params.forget_cell_mul_params,
      inter_gate_params.input_mul_params, updated_input_buffer,
      op_data.cell_state_info.quantized_cell_clip);

  /*Step3: update the hidden state */
  CellType* output_gate_output = kernel_content.buffer1;  // reuse buffer
  CalculateLstmGateInteger<ActivationType, WeightType, CellType, BiasType>(
      step_info, op_data.output_gate_parameters,
      // Input FC
      kernel_content.GetInternalTensor(tflite::kLstmInputTensor),
      kernel_content.GetInternalTensor(tflite::kLstmInputToOutputWeightsTensor),
      kernel_content.GetInternalTensor(tflite::kLstmOutputGateBiasTensor),
      // Recurrent FC
      kernel_content.HiddenStateTensor(),
      kernel_content.GetInternalTensor(
          tflite::kLstmRecurrentToOutputWeightsTensor),
      /*recurrent_bias*/ nullptr,
      // Output
      output_gate_output,
      // Scratch arrays
      gate_internal_buffer, kTfLiteActSigmoid);

  CellType* tanh_activated_cell_buffer =
      kernel_content.buffer0;  // reuse buffer
  tflite::lstm_internal::UpdateLstmHiddenInteger<CellType, ActivationType>(
      step_info, kernel_content.CellStateTensor(),
      kernel_content.HiddenStateTensor(), output_gate_output,
      inter_gate_params.output_mul_params,
      op_data.cell_state_info.cell_state_scale_power,
      tanh_activated_cell_buffer);
}

}  // namespace lstm_internal

// TODO (rewu): Modify the code to take into account of multi-step data
template <typename ActivationType, typename WeightType, typename CellType,
          typename BiasType>
TfLiteStatus EvalLstmInteger(const OpDataLSTM<CellType>& op_data,
                             LSTMKernelContents<CellType>& kernel_content) {
  ActivationType* output_ptr = tflite::micro::GetTensorData<ActivationType>(
      kernel_content.output_tensor);
  const auto& size_info = op_data.size_info;
  lstm_internal::LstmStepManager step_info(size_info);
  // time is the first dimention, enable batch computation
  if (size_info.time_major) {
    for (int t = 0; t < size_info.time_steps; t++) {
      // update cell and hidden states
      lstm_internal::LstmStepInteger<ActivationType, WeightType, CellType,
                                     BiasType>(step_info, op_data,
                                               kernel_content);
      // record the output (from the updated hidden state)
      std::memcpy(output_ptr + step_info.OutputOffset(),
                  tflite::micro::GetTensorData<ActivationType>(
                      kernel_content.HiddenStateTensor()),
                  size_info.batch_size * size_info.state_dimension *
                      sizeof(ActivationType));
      // prepare for the next time step
      step_info.UpdateTime();
    }
  } else {
    // batch first, unable to size the input data. single batch inference
    for (int b = 0; b < size_info.batch_size; b++) {
      for (int t = 0; t < size_info.time_steps; t++) {
        lstm_internal::LstmStepInteger<ActivationType, WeightType, CellType,
                                       BiasType>(step_info, op_data,
                                                 kernel_content);
        // record the output (from the updated hidden state)
        std::memcpy(output_ptr + step_info.OutputOffset(),
                    tflite::micro::GetTensorData<ActivationType>(
                        kernel_content.HiddenStateTensor()),
                    size_info.batch_size * size_info.state_dimension *
                        sizeof(ActivationType));
        // prepare for the next time step
        step_info.UpdateTime();
      }
      // prepare for the next batch
      step_info.UpdateBatch();
      step_info.ResetTime();
    }
  }
  return kTfLiteOk;
}
}  // namespace tflite

#endif  // TENSORFLOW_LITE_MICRO_KERNELS_LSTM_EVAL_16ACT_H_