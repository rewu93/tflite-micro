/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_MICRO_KERNELS_LSTM_EVAL_16ACT_TEST_H_
#define TENSORFLOW_LITE_MICRO_KERNELS_LSTM_EVAL_16ACT_TEST_H_

#include <algorithm>
#include <limits>

#include "tensorflow/lite/micro/kernels/lstm_eval_16act.h"
#include "tensorflow/lite/micro/kernels/lstm_eval_test.h"
namespace tflite {
namespace testing {
// since TfLiteContext is not available during the kernel test, we mimic (put
// into stack memory) CalculateOpDataFullyConnected in
// tensorflow/lite/micro/kernels/fully_connected_common.cc
template <typename CellType>
tflite::FullyConnectedParams CreateFCParams(
    const TensorQuantizationParameters& input_quant_params,
    const TensorQuantizationParameters& weight_quant_params,
    const float nonlinear_activation_input_scale) {
  OpDataFullyConnected data;
  const double input_product_scale =
      input_quant_params.scale * weight_quant_params.scale;
  double effective_scale =
      input_product_scale /
      static_cast<double>(nonlinear_activation_input_scale);

  QuantizeMultiplier(effective_scale, &data.output_multiplier,
                     &data.output_shift);

  data.input_zero_point = input_quant_params.zero_point;

  data.filter_zero_point = 0;  // symmetrically quantized
  data.output_zero_point = 0;  // symmetrically quantized

  data.output_activation_min = std::numeric_limits<CellType>::min();
  data.output_activation_max = std::numeric_limits<CellType>::max();

  return tflite::FullyConnectedParamsQuantized(data);
}

template <typename CellType>
tflite::GateParameters CreateGateParams(
    const TensorQuantizationParameters& input_quant_params,
    const TensorQuantizationParameters& hidden_state_quant_params,
    const GateQuantizationParameters& gate_quantization_settings,
    const float nonlinear_activation_input_scale) {
  tflite::GateParameters gate_params = {};
  gate_params.input_fc_params = CreateFCParams<CellType>(
      input_quant_params, gate_quantization_settings.activation_weight,
      nonlinear_activation_input_scale);
  gate_params.recurrent_fc_params = CreateFCParams<CellType>(
      hidden_state_quant_params, gate_quantization_settings.recurrent_weight,
      nonlinear_activation_input_scale);
  return gate_params;
}

template <typename OutputType>
tflite::ArithmeticParams CreateInterGateMulParams(const float input1_scale,
                                                  const float input2_scale,
                                                  const float output_scale,
                                                  const int output_zp = 0) {
  tflite::ArithmeticParams op_params = {};
  op_params.quantized_activation_min = std::numeric_limits<OutputType>::min();
  op_params.quantized_activation_max = std::numeric_limits<OutputType>::max();
  op_params.input1_offset = 0;
  op_params.input2_offset = 0;
  op_params.output_offset = output_zp;

  const double input_product_scale = input1_scale * input2_scale;
  double effective_scale =
      input_product_scale / static_cast<double>(output_scale);

  QuantizeMultiplier(effective_scale, &op_params.output_multiplier,
                     &op_params.output_shift);
  return op_params;
}

// Both preparation and invoke phases are tested here
template <typename ActivationType, typename BiasType, typename CellType,
          int batch_size, int state_dimension>
void TestGateOutputQuantized(
    const TfLiteEvalTensor* input, const TfLiteEvalTensor* input_weight,
    const TfLiteEvalTensor* input_bias,
    // Recurrent FC
    const TfLiteEvalTensor* recurrent, const TfLiteEvalTensor* recurrent_weight,
    const TfLiteEvalTensor* recurrent_bias,
    // Quantization settings
    const ModelQuantizationParameters& model_quantization_settings,
    const GateQuantizationParameters& gate_quantization_settings,
    // Result comparison
    TfLiteFusedActivation nonlinear_type, const float* expected_vals,
    float tolerance) {
  CellType gate_output[batch_size * state_dimension] = {};
  CellType fc_output_buffer[batch_size * state_dimension] = {};

  tflite::GateParameters gate_params = CreateGateParams<CellType>(
      model_quantization_settings.input_quantization_parameters,
      model_quantization_settings.hidden_quantization_parameters,
      gate_quantization_settings,
      model_quantization_settings.nonlinear_activation_input_scale);

  // only int8 weight is supported now
  tflite::lstm_internal::CalculateLstmGateInteger<ActivationType, int8_t,
                                                  CellType, BiasType>(
      gate_params,
      // Input FC
      input, input_weight, input_bias,
      // Recurrent FC
      recurrent, recurrent_weight, recurrent_bias,
      // Output
      gate_output,
      // Scratch arrays
      fc_output_buffer, nonlinear_type);

  float gate_output_float[batch_size * state_dimension] = {};
  Dequantize(gate_output, batch_size * state_dimension,
             model_quantization_settings.nonlinear_activation_output_scale, 0,
             gate_output_float);

  ValidateResultGoldens(expected_vals, gate_output_float,
                        batch_size * state_dimension, tolerance);
}

template <typename CellType, int batch_size, int input_dimension,
          int state_dimension>
void TestCellUpdateQuantized(
    TfLiteEvalTensor* cell_state,
    const GateOutputCheckData<batch_size * input_dimension,
                              batch_size * state_dimension>& gate_output_data,
    const ModelQuantizationParameters& quantization_settings,
    const CellType quantized_cell_clip, const float tolerance) {
  CellType quantized_forget_gate[batch_size * state_dimension] = {};
  tflite::Quantize(gate_output_data.expected_forget_gate_output,
                   quantized_forget_gate, batch_size * state_dimension,
                   quantization_settings.nonlinear_activation_output_scale, 0);

  CellType quantized_input_gate[batch_size * state_dimension] = {};
  tflite::Quantize(gate_output_data.expected_input_gate_output,
                   quantized_input_gate, batch_size * state_dimension,
                   quantization_settings.nonlinear_activation_output_scale, 0);

  CellType quantized_cell_gate[batch_size * state_dimension] = {};
  tflite::Quantize(gate_output_data.expected_cell_gate_output,
                   quantized_cell_gate, batch_size * state_dimension,
                   quantization_settings.nonlinear_activation_output_scale, 0);

  CellType buffer[batch_size * state_dimension] = {};

  auto forget_cell_mul_params = CreateInterGateMulParams<CellType>(
      quantization_settings.nonlinear_activation_output_scale,
      quantization_settings.cell_quantization_parameters.scale,
      quantization_settings.cell_quantization_parameters.scale);
  auto input_mul_params = CreateInterGateMulParams<CellType>(
      quantization_settings.nonlinear_activation_output_scale,
      quantization_settings.nonlinear_activation_output_scale,
      quantization_settings.cell_quantization_parameters.scale);

  tflite::lstm_internal::UpdateLstmCellInteger<CellType>(
      cell_state, quantized_forget_gate, quantized_input_gate,
      quantized_cell_gate, forget_cell_mul_params, input_mul_params, buffer,
      quantized_cell_clip);

  float cell_state_float[batch_size * state_dimension] = {};
  Dequantize(tflite::micro::GetTensorData<CellType>(cell_state),
             batch_size * state_dimension,
             quantization_settings.cell_quantization_parameters.scale,
             quantization_settings.cell_quantization_parameters.zero_point,
             cell_state_float);

  ValidateResultGoldens(gate_output_data.expected_updated_cell,
                        cell_state_float, batch_size * state_dimension,
                        tolerance);
}

template <typename ActivationType, typename CellType, int batch_size,
          int input_dimension, int state_dimension>
void TestHiddenStateUpdateQuantized(
    TfLiteEvalTensor* cell_state, TfLiteEvalTensor* hidden_state,
    const GateOutputCheckData<batch_size * input_dimension,
                              batch_size * state_dimension>& gate_output_data,
    const ModelQuantizationParameters& quantization_settings,
    const float tolerance) {
  CellType quantized_output_gate[batch_size * state_dimension] = {};
  tflite::Quantize(gate_output_data.expected_output_gate_output,
                   quantized_output_gate, batch_size * state_dimension,
                   quantization_settings.nonlinear_activation_output_scale, 0);

  CellType buffer[batch_size * state_dimension] = {};

  auto mul_params = CreateInterGateMulParams<ActivationType>(
      quantization_settings.nonlinear_activation_output_scale,
      quantization_settings.nonlinear_activation_output_scale,
      quantization_settings.hidden_quantization_parameters.scale,
      quantization_settings.hidden_quantization_parameters.zero_point);

  int cell_state_scale_power_buffer;
  tflite::CheckedLog2(quantization_settings.cell_quantization_parameters.scale,
                      &cell_state_scale_power_buffer);
  int32_t cell_state_scale_power = cell_state_scale_power_buffer;

  tflite::lstm_internal::UpdateLstmHiddenInteger<CellType, ActivationType>(
      cell_state, hidden_state, quantized_output_gate, mul_params,
      cell_state_scale_power, buffer);

  float hidden_state_float[batch_size * state_dimension] = {};
  Dequantize(tflite::micro::GetTensorData<ActivationType>(hidden_state),
             batch_size * state_dimension,
             quantization_settings.hidden_quantization_parameters.scale,
             quantization_settings.hidden_quantization_parameters.zero_point,
             hidden_state_float);

  ValidateResultGoldens(gate_output_data.expected_updated_hidden,
                        hidden_state_float, batch_size * state_dimension,
                        tolerance);
}

template <typename ActivationType, typename BiasType, typename CellType,
          int batch_size, int time_steps, int input_dimension,
          int state_dimension>
LSTMKernelContents<CellType> CreateLSTMKernelContent(
    const TfLiteLSTMParams& builtin_data, const float cell_state_scale,
    ModelContents<ActivationType, int8_t, BiasType, CellType, batch_size,
                  time_steps, input_dimension, state_dimension>&
        model_contents) {
  LSTMKernelContents<CellType> kernel_content;
  // Point to correct tensors
  kernel_content.internal_tensors[kLstmInputTensor] =
      model_contents.GetInternalTensor(kLstmInputTensor);
  kernel_content.internal_tensors[kLstmInputToInputWeightsTensor] =
      model_contents.GetInternalTensor(kLstmInputToInputWeightsTensor);
  kernel_content.internal_tensors[kLstmInputToForgetWeightsTensor] =
      model_contents.GetInternalTensor(kLstmInputToForgetWeightsTensor);
  kernel_content.internal_tensors[kLstmInputToCellWeightsTensor] =
      model_contents.GetInternalTensor(kLstmInputToCellWeightsTensor);
  kernel_content.internal_tensors[kLstmInputToOutputWeightsTensor] =
      model_contents.GetInternalTensor(kLstmInputToOutputWeightsTensor);
  kernel_content.internal_tensors[kLstmRecurrentToInputWeightsTensor] =
      model_contents.GetInternalTensor(kLstmRecurrentToInputWeightsTensor);
  kernel_content.internal_tensors[kLstmRecurrentToForgetWeightsTensor] =
      model_contents.GetInternalTensor(kLstmRecurrentToForgetWeightsTensor);
  kernel_content.internal_tensors[kLstmRecurrentToCellWeightsTensor] =
      model_contents.GetInternalTensor(kLstmRecurrentToCellWeightsTensor);
  kernel_content.internal_tensors[kLstmRecurrentToOutputWeightsTensor] =
      model_contents.GetInternalTensor(kLstmRecurrentToOutputWeightsTensor);
  kernel_content.internal_tensors[kLstmInputGateBiasTensor] =
      model_contents.GetInternalTensor(kLstmInputGateBiasTensor);
  kernel_content.internal_tensors[kLstmForgetGateBiasTensor] =
      model_contents.GetInternalTensor(kLstmForgetGateBiasTensor);
  kernel_content.internal_tensors[kLstmCellGateBiasTensor] =
      model_contents.GetInternalTensor(kLstmCellGateBiasTensor);
  kernel_content.internal_tensors[kLstmOutputGateBiasTensor] =
      model_contents.GetInternalTensor(kLstmOutputGateBiasTensor);
  kernel_content.internal_tensors[kLstmOutputStateTensor] =
      model_contents.GetInternalTensor(kLstmOutputStateTensor);
  kernel_content.internal_tensors[kLstmOutputGateBiasTensor] =
      model_contents.GetInternalTensor(kLstmOutputGateBiasTensor);
  kernel_content.internal_tensors[kLstmCellStateTensor] =
      model_contents.GetInternalTensor(kLstmCellStateTensor);
  // Not used internal tensors
  kernel_content.internal_tensors[kLstmCellToInputWeightsTensor] = nullptr;
  kernel_content.internal_tensors[kLstmCellToForgetWeightsTensor] = nullptr;
  kernel_content.internal_tensors[kLstmCellToOutputWeightsTensor] = nullptr;
  kernel_content.internal_tensors[kLstmProjectionWeightsTensor] = nullptr;
  kernel_content.internal_tensors[kLstmProjectionBiasTensor] = nullptr;
  kernel_content.internal_tensors[kLstmInputLayerNormCoefficientsTensor] =
      nullptr;
  kernel_content.internal_tensors[kLstmForgetLayerNormCoefficientsTensor] =
      nullptr;
  kernel_content.internal_tensors[kLstmInputLayerNormCoefficientsTensor] =
      nullptr;
  kernel_content.internal_tensors[kLstmCellLayerNormCoefficientsTensor] =
      nullptr;
  kernel_content.internal_tensors[kLstmOutputLayerNormCoefficientsTensor] =
      nullptr;
  // Output tensor
  kernel_content.output_tensor = model_contents.OutputTensor();

  // cell_state_scale_power: 2^-cell_state_scale_power = cell state scale
  int buffer;
  tflite::CheckedLog2(cell_state_scale, &buffer);
  kernel_content.cell_state_scale_power = buffer;
  // Cell state specifics
  kernel_content.cell_gate_nonlinear_type = builtin_data.activation;
  kernel_content.quantized_cell_clip = static_cast<CellType>(
      std::min(std::max(static_cast<double>(builtin_data.cell_clip) /
                            static_cast<double>(cell_state_scale),
                        -32768.0),
               32767.0));
  return kernel_content;
}

template <typename ActivationType, typename BiasType, typename CellType,
          int batch_size, int time_steps, int input_dimension,
          int state_dimension>
OpDataLSTM CreateLSTMOpData(
    const ModelQuantizationParameters& quantization_settings,
    ModelContents<ActivationType, int8_t, BiasType, CellType, batch_size,
                  time_steps, input_dimension, state_dimension>&
        model_contents) {
  OpDataLSTM op_data;
  // Gate Parameters
  op_data.forget_gate_parameters = CreateGateParams<CellType>(
      quantization_settings.input_quantization_parameters,
      quantization_settings.hidden_quantization_parameters,
      quantization_settings.forget_gate_quantization_parameters,
      quantization_settings.nonlinear_activation_input_scale);
  op_data.input_gate_parameters = CreateGateParams<CellType>(
      quantization_settings.input_quantization_parameters,
      quantization_settings.hidden_quantization_parameters,
      quantization_settings.input_gate_quantization_parameters,
      quantization_settings.nonlinear_activation_input_scale);
  op_data.cell_gate_parameters = CreateGateParams<CellType>(
      quantization_settings.input_quantization_parameters,
      quantization_settings.hidden_quantization_parameters,
      quantization_settings.cell_gate_quantization_parameters,
      quantization_settings.nonlinear_activation_input_scale);
  op_data.output_gate_parameters = CreateGateParams<CellType>(
      quantization_settings.input_quantization_parameters,
      quantization_settings.hidden_quantization_parameters,
      quantization_settings.output_gate_quantization_parameters,
      quantization_settings.nonlinear_activation_input_scale);
  // Inter gate multiplication parameters
  op_data.inter_gate_parameters.forget_cell_mul_params =
      CreateInterGateMulParams<CellType>(
          quantization_settings.nonlinear_activation_output_scale,
          quantization_settings.cell_quantization_parameters.scale,
          quantization_settings.cell_quantization_parameters.scale);
  op_data.inter_gate_parameters.input_mul_params =
      CreateInterGateMulParams<CellType>(
          quantization_settings.nonlinear_activation_output_scale,
          quantization_settings.nonlinear_activation_output_scale,
          quantization_settings.cell_quantization_parameters.scale);
  op_data.inter_gate_parameters.output_mul_params =
      CreateInterGateMulParams<ActivationType>(
          quantization_settings.nonlinear_activation_output_scale,
          quantization_settings.nonlinear_activation_output_scale,
          quantization_settings.hidden_quantization_parameters.scale,
          quantization_settings.hidden_quantization_parameters.zero_point);
  return op_data;
}

template <typename ActivationType, typename BiasType, typename CellType,
          int batch_size, int time_steps, int input_dimension,
          int state_dimension>
void TestOneStepLSTMInteger(
    const TfLiteLSTMParams& builtin_data,
    const ModelQuantizationParameters& quantization_settings,
    const GateOutputCheckData<batch_size * input_dimension,
                              batch_size * state_dimension>& gate_output_data,
    const float hidden_state_tolerance, const float cell_state_tolerance,
    /*can not be const, state will be updated*/
    ModelContents<ActivationType, int8_t, BiasType, CellType, batch_size,
                  time_steps, input_dimension, state_dimension>&
        model_contents) {
  // Mimicking the kernel preparation phase, model_contents approximate the node
  LSTMKernelContents<CellType> kernel_content = CreateLSTMKernelContent(
      builtin_data, quantization_settings.cell_quantization_parameters.scale,
      model_contents);
  // Scratch buffers on the stack
  CellType buffer0[batch_size * state_dimension] = {};
  kernel_content.buffer0 = buffer0;
  CellType buffer1[batch_size * state_dimension] = {};
  kernel_content.buffer1 = buffer1;
  CellType buffer2[batch_size * state_dimension] = {};
  kernel_content.buffer2 = buffer2;
  CellType buffer3[batch_size * state_dimension] = {};
  kernel_content.buffer3 = buffer3;

  OpDataLSTM op_data = CreateLSTMOpData(quantization_settings, model_contents);
  tflite::lstm_internal::LstmStepInteger<ActivationType, int8_t, CellType,
                                         BiasType>(op_data, kernel_content);

  float dequantized_hidden_state[batch_size * state_dimension] = {};
  Dequantize(tflite::micro::GetTensorData<ActivationType>(
                 kernel_content.HiddenStateTensor()),
             batch_size * state_dimension,
             quantization_settings.hidden_quantization_parameters.scale,
             quantization_settings.hidden_quantization_parameters.zero_point,
             dequantized_hidden_state);

  float dequantized_cell_state[batch_size * state_dimension] = {};
  Dequantize(
      tflite::micro::GetTensorData<CellType>(kernel_content.CellStateTensor()),
      batch_size * state_dimension,
      quantization_settings.cell_quantization_parameters.scale,
      quantization_settings.cell_quantization_parameters.zero_point,
      dequantized_cell_state);

  ValidateResultGoldens(gate_output_data.expected_updated_hidden,
                        dequantized_hidden_state, batch_size * state_dimension,
                        hidden_state_tolerance);
  ValidateResultGoldens(gate_output_data.expected_updated_cell,
                        dequantized_cell_state, batch_size * state_dimension,
                        cell_state_tolerance);
}

}  // namespace testing
}  // namespace tflite

#endif  // TENSORFLOW_LITE_MICRO_KERNELS_LSTM_EVAL_16ACT_TEST_H_