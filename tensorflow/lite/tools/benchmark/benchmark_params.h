/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_TOOLS_BENCHMARK_BENCHMARK_PARAMS_H_
#define TENSORFLOW_LITE_TOOLS_BENCHMARK_BENCHMARK_PARAMS_H_
#include "tensorflow/lite/tools/tool_params.h"

namespace tflite {
namespace benchmark {
using BenchmarkParam = tflite::tools::ToolParam;
using BenchmarkParams = tflite::tools::ToolParams;

#define LOG_BENCHMARK_PARAM(params, type, name, prefix, suffix, verbose) \
  do {                                                                   \
    TFLITE_MAY_LOG(INFO, verbose || params.HasValueSet<type>(name))      \
        << prefix << params.Get<type>(name) << suffix;                   \
  } while (0)
}  // namespace benchmark
}  // namespace tflite
#endif  // TENSORFLOW_LITE_TOOLS_BENCHMARK_BENCHMARK_PARAMS_H_
