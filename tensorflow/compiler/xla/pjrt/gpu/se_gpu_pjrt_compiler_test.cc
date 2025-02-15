/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/pjrt/gpu/se_gpu_pjrt_compiler.h"

#include <memory>
#include <vector>

#include "absl/status/status.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Parser/Parser.h"  // from @llvm-project
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/xla/pjrt/gpu/se_gpu_pjrt_client.h"
#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/tsl/platform/status_matchers.h"

namespace xla {
namespace {

using ::tsl::testing::StatusIs;

constexpr absl::string_view kProgram = R"(HloModule Computation

ENTRY Computation() -> s32[] {
  ROOT result = s32[] constant(2)
})";

constexpr absl::string_view mlir_str = R"mlir(
  module {
    func.func @main() -> tensor<i32> {
      %0 = mhlo.constant dense<2> : tensor<i32>
      return %0 : tensor<i32>
    }
  })mlir";

absl::StatusOr<xla::XlaComputation> GetXlaComputation(
    absl::string_view program) {
  TF_ASSIGN_OR_RETURN(auto hlo_module,
                      xla::ParseAndReturnUnverifiedModule(program, {}));

  return XlaComputation(hlo_module->ToProto());
}

TEST(StreamExecutorGpuCompilerTest, NoClientXla) {
  StreamExecutorGpuCompiler compiler;
  StreamExecutorGpuTopologyDescription topology(GpuId(), GpuName(),
                                                "Fake_device", {0, 1});

  TF_ASSERT_OK_AND_ASSIGN(auto computation, GetXlaComputation(kProgram));
  EXPECT_THAT(compiler.Compile(xla::CompileOptions(), computation, topology,
                               /*client=*/nullptr),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(StreamExecutorGpuCompilerTest, TopologyNotSameXla) {
  StreamExecutorGpuCompiler compiler;
  StreamExecutorGpuTopologyDescription topology(GpuId(), GpuName(),
                                                "Fake_device", {0, 1});

  TF_ASSERT_OK_AND_ASSIGN(
      auto client, GetStreamExecutorGpuClient(true, /*allocator_config=*/{},
                                              /*distributed_client=*/nullptr,
                                              /*node_id=*/0));
  TF_ASSERT_OK_AND_ASSIGN(auto computation, GetXlaComputation(kProgram));
  EXPECT_THAT(compiler.Compile(xla::CompileOptions(), computation, topology,
                               client.get()),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(StreamExecutorGpuCompilerTest, SuccessXla) {
  StreamExecutorGpuCompiler compiler;

  TF_ASSERT_OK_AND_ASSIGN(
      auto client, GetStreamExecutorGpuClient(true, /*allocator_config=*/{},
                                              /*distributed_client=*/nullptr,
                                              /*node_id=*/0));
  TF_ASSERT_OK_AND_ASSIGN(auto computation, GetXlaComputation(kProgram));
  TF_ASSERT_OK_AND_ASSIGN(auto topology, client->GetTopologyDescription());
  TF_ASSERT_OK_AND_ASSIGN(auto executable,
                          compiler.Compile(xla::CompileOptions(), computation,
                                           *topology, client.get()));
  const LoadOptions load_options;
  TF_ASSERT_OK_AND_ASSIGN(auto loaded_executable,
                          client->Load(std::move(executable), load_options));

  TF_ASSERT_OK_AND_ASSIGN(
      auto result, loaded_executable->Execute(/*argument_handles=*/{{}}, {}));

  ASSERT_EQ(result.size(), 1);
  std::vector<std::unique_ptr<xla::PjRtBuffer>>& result_buffers = result[0];
  ASSERT_EQ(result_buffers.size(), 1);
  TF_ASSERT_OK_AND_ASSIGN(std::shared_ptr<xla::Literal> result_literal,
                          result_buffers[0]->ToLiteralSync());
  EXPECT_TRUE(
      LiteralTestUtil::Equal(LiteralUtil::CreateR0(2), *result_literal));
}

TEST(StreamExecutorGpuCompilerTest, NoClientMlir) {
  StreamExecutorGpuCompiler compiler;

  mlir::MLIRContext context;
  context.loadDialect<mlir::mhlo::MhloDialect, mlir::func::FuncDialect>();

  auto mlir_module =
      mlir::parseSourceString<mlir::ModuleOp>(mlir_str, &context);

  StreamExecutorGpuTopologyDescription topology(GpuId(), GpuName(),
                                                "Fake_device", {0, 1});

  EXPECT_THAT(
      compiler.Compile(xla::CompileOptions(), mlir_module.get(), topology,
                       /*client=*/nullptr),
      StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(StreamExecutorGpuCompilerTest, TopologyNotSameMlir) {
  StreamExecutorGpuCompiler compiler;

  mlir::MLIRContext context;
  context.loadDialect<mlir::mhlo::MhloDialect, mlir::func::FuncDialect>();

  auto mlir_module =
      mlir::parseSourceString<mlir::ModuleOp>(mlir_str, &context);

  StreamExecutorGpuTopologyDescription topology(GpuId(), GpuName(),
                                                "Fake_device", {0, 1});

  TF_ASSERT_OK_AND_ASSIGN(
      auto client, GetStreamExecutorGpuClient(true, /*allocator_config=*/{},
                                              /*distributed_client=*/nullptr,
                                              /*node_id=*/0));
  EXPECT_THAT(compiler.Compile(xla::CompileOptions(), mlir_module.get(),
                               topology, client.get()),
              StatusIs(absl::StatusCode::kUnimplemented));
}

TEST(StreamExecutorGpuCompilerTest, SuccessMlir) {
  StreamExecutorGpuCompiler compiler;

  mlir::MLIRContext context;
  context.loadDialect<mlir::mhlo::MhloDialect, mlir::func::FuncDialect>();

  auto mlir_module =
      mlir::parseSourceString<mlir::ModuleOp>(mlir_str, &context);

  TF_ASSERT_OK_AND_ASSIGN(
      auto client, GetStreamExecutorGpuClient(true, /*allocator_config=*/{},
                                              /*distributed_client=*/nullptr,
                                              /*node_id=*/0));
  TF_ASSERT_OK_AND_ASSIGN(auto topology, client->GetTopologyDescription());
  TF_ASSERT_OK_AND_ASSIGN(
      auto executable,
      compiler.Compile(xla::CompileOptions(), mlir_module.get(), *topology,
                       client.get()));
  const LoadOptions load_options;
  TF_ASSERT_OK_AND_ASSIGN(auto loaded_executable,
                          client->Load(std::move(executable), load_options));

  TF_ASSERT_OK_AND_ASSIGN(
      auto result, loaded_executable->Execute(/*argument_handles=*/{{}}, {}));

  ASSERT_EQ(result.size(), 1);
  std::vector<std::unique_ptr<xla::PjRtBuffer>>& result_buffers = result[0];
  ASSERT_EQ(result_buffers.size(), 1);
  TF_ASSERT_OK_AND_ASSIGN(std::shared_ptr<xla::Literal> result_literal,
                          result_buffers[0]->ToLiteralSync());
  EXPECT_TRUE(
      LiteralTestUtil::Equal(LiteralUtil::CreateR0(2), *result_literal));
}

}  // namespace
}  // namespace xla
