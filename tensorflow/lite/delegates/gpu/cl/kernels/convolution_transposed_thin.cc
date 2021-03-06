/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/delegates/gpu/cl/kernels/convolution_transposed_thin.h"

#include <string>
#include <utility>
#include <vector>

#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"
#include "tensorflow/lite/delegates/gpu/cl/precision.h"

namespace tflite {
namespace gpu {
namespace cl {
namespace {

std::string GenerateConvolutionTransposedCode(const OperationDef& op_def,
                                              int src_depth, int dst_channels,
                                              const int2& kernel_size,
                                              Arguments* args) {
  args->AddObjectRef(
      "src_tensor", AccessType::READ,
      absl::make_unique<TensorDescriptor>(op_def.src_tensors[0]));
  args->AddObjectRef(
      "dst_tensor", AccessType::WRITE,
      absl::make_unique<TensorDescriptor>(op_def.dst_tensors[0]));

  const std::string channel_x = dst_channels == 1 ? "" : ".x";
  const std::vector<std::string> postfix = {channel_x, ".y", ".z", ".w"};
  const std::vector<std::string> channel = {".x", ".y", ".z", ".w"};

  const std::string type_postfix =
      dst_channels == 1 ? "" : std::to_string(dst_channels);

  std::string accum_type;

  switch (op_def.precision) {
    case CalculationsPrecision::F32:
    case CalculationsPrecision::F32_F16:
      accum_type = "float" + type_postfix;
      break;
    case CalculationsPrecision::F16:
      accum_type = "half" + type_postfix;
      break;
  }

  std::string c = GetCommonDefines(op_def.precision);
  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  if (op_def.IsBatchSupported()) {
    c += "  int linear_id = get_global_id(0);\n";
    c += "  int X = linear_id / args.dst_tensor.Batch();\n";
    c += "  int B = linear_id % args.dst_tensor.Batch();\n";
    c += "  args.dst_tensor.SetBatchRef(B);\n";
    c += "  args.src_tensor.SetBatchRef(B);\n";
  } else {
    c += "  int X = get_global_id(0);\n";
  }
  c += "  int Y = get_global_id(1);\n";
  c += "  if (X >= args.src_tensor.Width() || Y >= args.src_tensor.Height()) "
       "return;\n";
  c += "  " + accum_type + " r[" + std::to_string(kernel_size.y) + "][" +
       std::to_string(kernel_size.x) + "];\n";
  c += "  {\n";
  c += "  FLT4 src = args.src_tensor.Read(X, Y, 0);\n";
  int index = 0;
  for (int y = 0; y < kernel_size.y; ++y) {
    for (int x = 0; x < kernel_size.x; ++x) {
      std::string r_s =
          "  r[" + std::to_string(y) + "][" + std::to_string(x) + "]";
      for (int d = 0; d < dst_channels; ++d) {
        c += r_s + postfix[d] + " = dot(src, args.weights.Read(" +
             std::to_string(index) + "));\n";
        index++;
      }
    }
  }
  c += "  }\n";
  for (int i = 1; i < src_depth; ++i) {
    c += "  if (X > " + std::to_string(-i) +
         ") {  // always true, to reduce registers usage\n";
    c +=
        "  FLT4 src = args.src_tensor.Read(X, Y, " + std::to_string(i) + ");\n";
    for (int y = 0; y < kernel_size.y; ++y) {
      for (int x = 0; x < kernel_size.x; ++x) {
        std::string r_s =
            "  r[" + std::to_string(y) + "][" + std::to_string(x) + "]";
        for (int d = 0; d < dst_channels; ++d) {
          c += r_s + postfix[d] + " += dot(src, args.weights.Read(" +
               std::to_string(index) + "));\n";
          index++;
        }
      }
    }
    c += "  }\n";
  }
  c += "  X *= " + std::to_string(kernel_size.x) + ";\n";
  c += "  Y *= " + std::to_string(kernel_size.y) + ";\n";
  for (int y = 0; y < kernel_size.y; ++y) {
    for (int x = 0; x < kernel_size.x; ++x) {
      const std::string x_coord = "X + " + std::to_string(x);
      const std::string y_coord = "Y + " + std::to_string(y);
      c += "  if (" + x_coord + " < args.dst_tensor.Width() && " + y_coord +
           " < args.dst_tensor.Height()) {\n";
      c += "    FLT4 result = args.weights.Read(" + std::to_string(index) +
           ");\n";
      for (int d = 0; d < dst_channels; ++d) {
        c += "    result" + channel[d] + " += r[" + std::to_string(y) + "][" +
             std::to_string(x) + "]" + postfix[d] + ";\n";
      }
      c += "    args.dst_tensor.Write(result, " + x_coord + ", " + y_coord +
           ", 0);\n";
      c += "  }\n";
    }
  }
  c += "}\n";

  return c;
}
}  // namespace

ConvolutionTransposedThin::ConvolutionTransposedThin(
    const OperationDef& definition, const ConvolutionTransposedAttributes& attr)
    : GPUOperation(definition),
      kernel_size_(attr.weights.shape.w, attr.weights.shape.h),
      src_channels_(attr.weights.shape.i),
      dst_channels_(attr.weights.shape.o) {}

ConvolutionTransposedThin::ConvolutionTransposedThin(
    ConvolutionTransposedThin&& operation)
    : GPUOperation(std::move(operation)),
      kernel_size_(operation.kernel_size_),
      src_channels_(operation.src_channels_),
      dst_channels_(operation.dst_channels_),
      kernel_(std::move(operation.kernel_)),
      work_group_size_(operation.work_group_size_) {}

ConvolutionTransposedThin& ConvolutionTransposedThin::operator=(
    ConvolutionTransposedThin&& operation) {
  if (this != &operation) {
    std::swap(kernel_size_, operation.kernel_size_);
    std::swap(src_channels_, operation.src_channels_);
    std::swap(dst_channels_, operation.dst_channels_);
    kernel_ = std::move(operation.kernel_);
    std::swap(work_group_size_, operation.work_group_size_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

absl::Status ConvolutionTransposedThin::Compile(
    const CreationContext& creation_context) {
  std::string code = GenerateConvolutionTransposedCode(
      definition_, DivideRoundUp(src_channels_, 4), dst_channels_, kernel_size_,
      &args_);
  std::string element_wise_code;
  RETURN_IF_ERROR(
      MergeOperations(linked_operations_, &args_, &element_wise_code));
  RETURN_IF_ERROR(args_.TransformToCLCode(creation_context.device->GetInfo(),
                                          {{"dst_tensor", element_wise_code}},
                                          &code));

  std::vector<CompilerOptions> options;
  if (definition_.precision == CalculationsPrecision::F16 &&
      creation_context.device->IsAdreno3xx()) {
    options.push_back(CompilerOptions::ADRENO_FULL_SIMD_LINE);
  }

  return creation_context.cache->GetOrCreateCLKernel(
      code, "main_function", *creation_context.context,
      *creation_context.device, &kernel_);
}

absl::Status ConvolutionTransposedThin::BindArguments() {
  RETURN_IF_ERROR(args_.SetObjectRef("src_tensor", src_[0]));
  RETURN_IF_ERROR(args_.SetObjectRef("dst_tensor", dst_[0]));
  RETURN_IF_ERROR(SetArguments(linked_operations_, &args_));
  return args_.Bind(kernel_.kernel());
}

int3 ConvolutionTransposedThin::GetGridSize() const {
  const int grid_x = src_[0]->Width() * dst_[0]->Batch();
  const int grid_y = src_[0]->Height();
  const int grid_z = 1;
  return int3(grid_x, grid_y, grid_z);
}

absl::Status ConvolutionTransposedThin::Tune(const TuningParameters& params) {
  RETURN_IF_ERROR(BindArguments());
  return GetBestWorkGroup(params, kernel_, GetGridSize(), &work_group_size_);
}

absl::Status ConvolutionTransposedThin::AddToQueue(CLCommandQueue* queue) {
  RETURN_IF_ERROR(BindArguments());
  return queue->DispatchImplicit(kernel_, GetGridSize(), work_group_size_);
}

bool IsConvolutionTransposedThinSupported(
    const CLDevice& device, const ConvolutionTransposedAttributes& attr) {
  return attr.weights.shape.o <= 4 && attr.weights.shape.w == attr.stride.w &&
         attr.weights.shape.h == attr.stride.h &&
         attr.padding.prepended.w == 0 && attr.padding.prepended.h == 0 &&
         attr.padding.appended.w == 0 && attr.padding.appended.h == 0;
}

absl::Status CreateConvolutionTransposedThin(
    const CreationContext& creation_context, const OperationDef& definition,
    const ConvolutionTransposedAttributes& attr,
    ConvolutionTransposedThin* result) {
  if (!IsConvolutionTransposedThinSupported(*creation_context.device, attr)) {
    return absl::InvalidArgumentError(
        "ConvolutionTransposedThin doesn't support this attributes");
  }
  *result = ConvolutionTransposedThin(definition, attr);
  RETURN_IF_ERROR(
      result->UploadData(attr.weights, attr.bias, creation_context.context));
  return absl::OkStatus();
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
