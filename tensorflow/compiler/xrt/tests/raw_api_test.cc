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

#include <memory>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "tensorflow/cc/client/client_session.h"
#include "tensorflow/cc/framework/ops.h"
#include "tensorflow/cc/framework/scope.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/compiler/tf2xla/literal_util.h"
#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/compiler/xrt/cc/ops/xrt_compile_ops.h"
#include "tensorflow/compiler/xrt/cc/ops/xrt_execute_op.h"
#include "tensorflow/compiler/xrt/cc/ops/xrt_state_ops.h"
#include "tensorflow/compiler/xrt/xrt.pb.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/command_line_flags.h"

namespace tensorflow {
namespace {

string* xla_test_device_ptr;  // initial value set in main()
string* xla_platform_ptr;     // initial value set in main()

string DeviceFromFlag() {
  string xla_test_device = *xla_test_device_ptr;
  return absl::StrCat("/device:", xla_test_device, ":0");
}

std::vector<int> GetAttrLayout(absl::Span<const int64> minor_to_mayor) {
  std::vector<int> layout;
  for (auto dim : minor_to_mayor) {
    layout.push_back(static_cast<int>(dim));
  }
  return layout;
}

xla::LiteralProto TwoElementTuple() {
  auto array = xla::LiteralUtil::CreateR1<float>({1.0f, 3.0f});
  auto matrix = xla::LiteralUtil::CreateR2({{4, 5}, {6, 7}});
  auto tuple = xla::LiteralUtil::MakeTuple({&array, &matrix});
  return tuple.ToProto();
}

xla::LiteralProto ScalarLiteral() {
  auto scalar = xla::LiteralUtil::CreateR0<float>(12.0f);
  return scalar.ToProto();
}

xla::LiteralProto NestedTuple() {
  auto array = xla::LiteralUtil::CreateR1<float>({1.0f, 3.0f});
  auto matrix = xla::LiteralUtil::CreateR2({{4, 5}, {6, 7}});
  auto tuple = xla::LiteralUtil::MakeTuple({&array, &matrix});
  auto scalar = xla::LiteralUtil::CreateR0<float>(12.0f);
  auto nested = xla::LiteralUtil::MakeTuple({&tuple, &scalar});
  return nested.ToProto();
}

xla::LiteralProto MakeTuple0() {
  auto scalar = xla::LiteralUtil::CreateR0<float>(12.0f);
  auto array = xla::LiteralUtil::CreateR1<float>({1.0f, 3.0f});
  auto matrix = xla::LiteralUtil::CreateR2({{4, 5}, {6, 7}});
  auto tuple = xla::LiteralUtil::MakeTuple({&array, &matrix});
  auto nested0 = xla::LiteralUtil::MakeTuple({&scalar, &tuple});
  auto nested1 = xla::LiteralUtil::MakeTuple({&scalar, &nested0});
  return nested1.ToProto();
}

xla::LiteralProto FloatVector(absl::Span<const float> v) {
  auto array = xla::LiteralUtil::CreateR1<float>(v);
  return array.ToProto();
}

xla::LiteralProto FloatMatrix(
    std::initializer_list<std::initializer_list<float>> v,
    const xla::Layout& layout) {
  auto array = xla::LiteralUtil::CreateR2WithLayout<float>(v, layout);
  return array.ToProto();
}

xla::Literal ReadOutputLiteral(const std::vector<Tensor>& outputs, size_t idx) {
  xla::LiteralProto response;
  CHECK(response.ParseFromString(outputs[idx].scalar<string>()()));
  return xla::Literal::CreateFromProto(response).ValueOrDie();
}

bool CompareLiteralProtos(const xla::LiteralProto& a,
                          const xla::LiteralProto& b) {
  auto l_a = xla::Literal::CreateFromProto(a).ValueOrDie();
  auto l_b = xla::Literal::CreateFromProto(b).ValueOrDie();
  bool equal = l_a == l_b;
  if (!equal) {
    LOG(INFO) << "LiteralProtos don't match:\n"
              << a.DebugString() << "\n!=\n"
              << b.DebugString();
  }
  return equal;
}

bool CompareLiteralToLiteralProto(const xla::Literal& a,
                                  const xla::LiteralProto& b) {
  auto l_b = xla::Literal::CreateFromProto(b).ValueOrDie();
  bool equal = a == l_b;
  if (!equal) {
    LOG(INFO) << "Literal and LiteralProto don't match:\n"
              << a.ToProto().DebugString() << "\n!=\n"
              << b.DebugString();
  }
  return equal;
}

bool CompareLiterals(const xla::Literal& a, const xla::Literal& b) {
  bool equal = a == b;
  if (!equal) {
    LOG(INFO) << "Literals don't match:\n"
              << a.ToProto().DebugString() << "\n!=\n"
              << b.ToProto().DebugString();
  }
  return equal;
}

xla::XlaComputation OnePlusTwo() {
  xla::XlaBuilder builder("OnePlusTwo");
  auto c0 = xla::ConstantR0(&builder, 1.0f);
  auto c1 = xla::ConstantR0(&builder, 2.0f);
  xla::Add(c0, c1);
  return builder.Build().ValueOrDie();
}

xla::XlaComputation AddAndScale() {
  xla::XlaBuilder builder("AddAndScale");
  auto p0 = xla::Parameter(&builder, 0,
                           xla::ShapeUtil::MakeShape(xla::F32, {2}), "P0");
  auto p1 = xla::Parameter(&builder, 1,
                           xla::ShapeUtil::MakeShape(xla::F32, {2}), "P1");
  auto sum = xla::Add(p0, p1);
  auto c = xla::ConstantR0<float>(&builder, 3.0f);
  xla::Mul(sum, c);
  return builder.Build().ValueOrDie();
}

xla::XlaComputation Dot() {
  xla::XlaBuilder builder("Dot");
  auto p0 = xla::Parameter(
      &builder, 0,
      xla::ShapeUtil::MakeShapeWithLayout(xla::F32, {2, 2}, {0, 1}), "P0");
  auto p1 = xla::Parameter(
      &builder, 1,
      xla::ShapeUtil::MakeShapeWithLayout(xla::F32, {2, 1}, {0, 1}), "P1");
  xla::DotDimensionNumbers ddn;
  ddn.add_lhs_contracting_dimensions(1);
  ddn.add_rhs_contracting_dimensions(0);
  xla::DotGeneral(p0, p1, ddn);
  return builder.Build().ValueOrDie();
}

xla::XlaComputation AddS64() {
  xla::XlaBuilder builder("AddS64");
  auto p0 = xla::Parameter(&builder, 0, xla::ShapeUtil::MakeShape(xla::S64, {}),
                           "P0");
  auto p1 = xla::Parameter(&builder, 1, xla::ShapeUtil::MakeShape(xla::S64, {}),
                           "P1");
  xla::Add(p0, p1);
  return builder.Build().ValueOrDie();
}

xla::XlaComputation AddAndTuple() {
  xla::XlaBuilder builder("AddAndTuple");
  auto p0 = xla::Parameter(&builder, 0,
                           xla::ShapeUtil::MakeShape(xla::F32, {2}), "P0");
  auto p1 = xla::Parameter(&builder, 1,
                           xla::ShapeUtil::MakeShape(xla::F32, {2}), "P1");
  auto sum = xla::Add(p0, p1);
  xla::Tuple(&builder, {sum});
  return builder.Build().ValueOrDie();
}

xla::XlaComputation AddAndSubTuple() {
  xla::XlaBuilder builder("AddAndSubTuple");
  auto p0 = xla::Parameter(&builder, 0, xla::ShapeUtil::MakeShape(xla::F32, {}),
                           "P0");
  auto p1 = xla::Parameter(&builder, 1, xla::ShapeUtil::MakeShape(xla::F32, {}),
                           "P1");
  auto sum = xla::Add(p0, p1);
  auto sub = xla::Sub(p0, p1);
  xla::Tuple(&builder, {sum, sub});
  return builder.Build().ValueOrDie();
}

void StoreComputationSnapshot(const xla::XlaComputation& computation,
                              xla::HloSnapshot* dst) {
  auto snapshot = computation.Snapshot().ValueOrDie();
  *dst = *snapshot;
}

xla::ProgramShape XlaCompiledProgramShape(
    const xla::XlaComputation& computation,
    const xla::ProgramShape& input_program_shape) {
  se::Platform* platform =
      xla::PlatformUtil::GetPlatform(*xla_platform_ptr).ValueOrDie();
  xla::LocalClient* client =
      xla::ClientLibrary::GetOrCreateLocalClient(platform).ValueOrDie();
  xla::ExecutableBuildOptions exec_options;
  exec_options.set_result_layout(input_program_shape.result());
  std::vector<const xla::Shape*> parameters_shapes;
  for (int64 i = 0; i < input_program_shape.parameters_size(); ++i) {
    parameters_shapes.push_back(&input_program_shape.parameters(i));
  }
  auto local_executable =
      client->Compile(computation, parameters_shapes, exec_options)
          .ValueOrDie();
  return local_executable->executable()
      ->module()
      .entry_computation()
      ->ComputeProgramShape();
}

TEST(RawApiTest, AllocFromTensor) {
  xla::Literal literal =
      xla::LiteralUtil::CreateR2<float>({{4.0f, 5.0f}, {6.0f, 7.0f}});
  Tensor tensor;
  TF_ASSERT_OK(LiteralToHostTensor(literal, DT_FLOAT, &tensor));

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  std::vector<int> layout =
      GetAttrLayout(literal.shape().layout().minor_to_major());
  ops::XRTAllocateFromTensor::Attrs alloc_attrs =
      ops::XRTAllocateFromTensor::Layouts(layout);
  auto handle =
      ops::XRTAllocateFromTensor(root, {tensor}, {tensor.shape()}, alloc_attrs);
  auto read_back = ops::XRTReadLiteralAndRelease(root, handle);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));
  EXPECT_EQ(outputs.size(), 1);

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralToLiteralProto(literal, response));
}

TEST(RawApiTest, AllocFromTensorTuple) {
  xla::Literal literal0 =
      xla::LiteralUtil::CreateR2<float>({{4.0f, 5.0f}, {6.0f, 7.0f}});
  xla::Literal literal1 =
      xla::LiteralUtil::CreateR2<float>({{14.0f, -5.0f}, {16.0f, 17.0f}});
  xla::Literal literal = xla::LiteralUtil::MakeTuple({&literal0, &literal1});
  Tensor tensor0;
  TF_ASSERT_OK(LiteralToHostTensor(literal0, DT_FLOAT, &tensor0));
  Tensor tensor1;
  TF_ASSERT_OK(LiteralToHostTensor(literal1, DT_FLOAT, &tensor1));

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  std::vector<int> layout = GetShapeLayoutVector(literal.shape()).ValueOrDie();
  ops::XRTAllocateFromTensor::Attrs alloc_attrs =
      ops::XRTAllocateFromTensor::Layouts(layout);
  auto handle = ops::XRTAllocateFromTensor(root, {tensor0, tensor1},
                                           {tensor0.shape(), tensor1.shape()},
                                           alloc_attrs);
  auto read_back = ops::XRTReadLiteralAndRelease(root, handle);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));
  EXPECT_EQ(outputs.size(), 1);

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralToLiteralProto(literal, response));
}

TEST(RawApiTest, AllocFromTensorTupleSingle) {
  xla::Literal literal0 =
      xla::LiteralUtil::CreateR2<float>({{4.0f, 5.0f}, {6.0f, 7.0f}});
  xla::Literal literal = xla::LiteralUtil::MakeTuple({&literal0});
  Tensor tensor0;
  TF_ASSERT_OK(LiteralToHostTensor(literal0, DT_FLOAT, &tensor0));

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  std::vector<int> layout = GetShapeLayoutVector(literal.shape()).ValueOrDie();
  ops::XRTAllocateFromTensor::Attrs alloc_attrs =
      ops::XRTAllocateFromTensor::Layouts(layout).MakeTuple(true);
  auto handle = ops::XRTAllocateFromTensor(root, {tensor0}, {tensor0.shape()},
                                           alloc_attrs);
  auto read_back = ops::XRTReadLiteralAndRelease(root, handle);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));
  EXPECT_EQ(outputs.size(), 1);

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralToLiteralProto(literal, response));
}

TEST(RawApiTest, AllocFromTensorRelayout) {
  xla::Literal literal =
      xla::LiteralUtil::CreateR2<float>({{4.0f, 5.0f}, {6.0f, 7.0f}});
  Tensor tensor;
  TF_ASSERT_OK(LiteralToHostTensor(literal, DT_FLOAT, &tensor));

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  // Use inverse array layout with the tensor data above.
  std::vector<int> layout({0, 1});
  ops::XRTAllocateFromTensor::Attrs alloc_attrs =
      ops::XRTAllocateFromTensor::Layouts(layout);
  auto handle =
      ops::XRTAllocateFromTensor(root, {tensor}, {tensor.shape()}, alloc_attrs);
  auto read_back = ops::XRTReadLiteralAndRelease(root, handle);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));
  EXPECT_EQ(outputs.size(), 1);

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));
  // We have sent literal's data (in array layout) with a attribute layout
  // {0,1}, so the expected literal read from device needs to be changed
  // accordingly.
  xla::Literal expected_literal =
      xla::LiteralUtil::CreateR2<float>({{4.0f, 6.0f}, {5.0f, 7.0f}});
  EXPECT_TRUE(CompareLiteralToLiteralProto(expected_literal, response));
}

TEST(RawApiTest, AllocAndRewrite) {
  xrt::XLAAllocation alloc;
  *alloc.mutable_value() =
      xla::LiteralUtil::CreateR2({{4, 5}, {6, 7}}).ToProto();

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto value =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc.SerializeAsString());
  auto handle = ops::XRTAllocate(root, value);
  auto read_back = ops::XRTReadLiteral(root, handle);
  TF_ASSERT_OK(root.status());

  tensorflow::ClientSession session(root);
  std::vector<tensorflow::Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back, handle}, &outputs));
  EXPECT_EQ(outputs.size(), 2);

  int64 allocation_handle = outputs[1].scalar<int64>()();
  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralProtos(alloc.value(), response));
  outputs.clear();

  xla::LiteralProto new_literal =
      xla::LiteralUtil::CreateR2({{9, 2}, {4, 1}}).ToProto();
  auto new_value = ops::Const(root.WithDevice("/device:CPU:0"),
                              new_literal.SerializeAsString());
  auto write_op =
      ops::XRTWriteLiteral(root, Input(allocation_handle), new_value);
  TF_ASSERT_OK(root.status());
  TF_EXPECT_OK(session.Run({write_op}, &outputs));
  EXPECT_EQ(outputs.size(), 1);
  EXPECT_EQ(allocation_handle, outputs[0].scalar<int64>()());
  outputs.clear();

  auto read_after_write = ops::XRTReadLiteral(root, Input(allocation_handle));
  TF_EXPECT_OK(session.Run({read_after_write}, &outputs));
  EXPECT_EQ(outputs.size(), 1);

  xla::LiteralProto new_response;
  EXPECT_TRUE(new_response.ParseFromString(outputs[0].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralProtos(new_literal, new_response));

  Tensor release_tensor(DT_INT64, TensorShape({1}));
  release_tensor.flat<int64>()(0) = allocation_handle;

  auto release = ops::XRTReleaseAllocationHandle(root, release_tensor);
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(), {}, {release},
                           &outputs));
}

TEST(RawApiTest, AllocReleaseMany) {
  xrt::XLAAllocation alloc1;
  *alloc1.mutable_value() =
      xla::LiteralUtil::CreateR2({{4, 5}, {6, 7}}).ToProto();
  xrt::XLAAllocation alloc2;
  *alloc2.mutable_value() =
      xla::LiteralUtil::CreateR2({{6, 7}, {4, 5}}).ToProto();

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto value1 =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc1.SerializeAsString());
  auto value2 =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc2.SerializeAsString());
  auto handle1 = ops::XRTAllocate(root, value1);
  auto handle2 = ops::XRTAllocate(root, value2);
  TF_ASSERT_OK(root.status());

  tensorflow::ClientSession session(root);
  std::vector<tensorflow::Tensor> outputs;
  TF_EXPECT_OK(session.Run({handle1, handle2}, &outputs));
  EXPECT_EQ(outputs.size(), 2);

  int64 allocation_handle1 = outputs[0].scalar<int64>()();
  int64 allocation_handle2 = outputs[1].scalar<int64>()();

  Tensor release_tensor(DT_INT64, TensorShape({2}));
  release_tensor.flat<int64>()(0) = allocation_handle1;
  release_tensor.flat<int64>()(1) = allocation_handle2;

  auto release = ops::XRTReleaseAllocationHandle(root, release_tensor);
  outputs.clear();
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(), {}, {release},
                           &outputs));
}

TEST(RawApiTest, CompileAndReleaseMany) {
  xrt::XLAComputation c1;
  auto config1 = c1.mutable_config();
  auto shapes1 = config1->mutable_program_shape();
  *shapes1->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes1->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes1->mutable_result() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  StoreComputationSnapshot(AddAndScale(), c1.mutable_hlo_snapshot());

  xrt::XLAComputation c2;
  auto config2 = c2.mutable_config();
  auto shapes2 = config2->mutable_program_shape();
  *shapes2->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes2->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes2->mutable_result() =
      xla::ShapeUtil::MakeTupleShape({xla::ShapeUtil::MakeShape(xla::F32, {2})})
          .ToProto();
  StoreComputationSnapshot(AddAndTuple(), c2.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(false);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation1 =
      ops::Const(root.WithDevice("/device:CPU:0"), c1.SerializeAsString());
  auto c_handle1 = ops::XRTCompile(root, computation1);
  auto computation2 =
      ops::Const(root.WithDevice("/device:CPU:0"), c2.SerializeAsString());
  auto c_handle2 = ops::XRTCompile(root, computation2);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({c_handle1.handle, c_handle2.handle}, &outputs));
  EXPECT_EQ(outputs.size(), 2);

  int64 compilation_handle1 = outputs[0].scalar<int64>()();
  int64 compilation_handle2 = outputs[1].scalar<int64>()();

  Tensor release_tensor(DT_INT64, TensorShape({2}));
  release_tensor.flat<int64>()(0) = compilation_handle1;
  release_tensor.flat<int64>()(1) = compilation_handle2;

  auto release = ops::XRTReleaseCompilationHandle(root, release_tensor);
  outputs.clear();
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(), {}, {release},
                           &outputs));
}

TEST(RawApiTest, AllocAndClearAll) {
  xrt::XLAAllocation alloc;
  *alloc.mutable_value() =
      xla::LiteralUtil::CreateR2({{4, 5}, {6, 7}}).ToProto();

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto value =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc.SerializeAsString());
  auto handle = ops::XRTAllocate(root, value);
  TF_ASSERT_OK(root.status());

  tensorflow::ClientSession session(root);
  std::vector<tensorflow::Tensor> outputs;
  TF_EXPECT_OK(session.Run({handle}, &outputs));
  EXPECT_EQ(outputs.size(), 1);

  int64 allocation_handle = outputs[0].scalar<int64>()();

  auto clear_all = ops::XRTReleaseAllAllocations(root);

  outputs.clear();
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(), {},
                           {clear_all}, &outputs));
  EXPECT_EQ(outputs.size(), 0);

  auto read_after_clear = ops::XRTReadLiteral(root, Input(allocation_handle));
  EXPECT_EQ(session.Run({read_after_clear}, &outputs).code(),
            tensorflow::error::Code::NOT_FOUND);
}

TEST(RawApiTest, ReadAndWriteState) {
  xrt::XLAAllocation alloc;
  *alloc.mutable_value() = TwoElementTuple();

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto value =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc.SerializeAsString());
  auto handle = ops::XRTAllocate(root, value);
  auto read_back = ops::XRTReadLiteral(root, handle);
  auto release = ops::XRTReleaseAllocationHandle(
      root.WithControlDependencies(read_back), handle);
  TF_ASSERT_OK(root.status());

  tensorflow::ClientSession session(root);
  std::vector<tensorflow::Tensor> outputs;
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(), {read_back},
                           {release}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));

  EXPECT_TRUE(CompareLiteralProtos(alloc.value(), response));
}

TEST(RawApiTest, ReadAndWriteStateAutoFree) {
  xrt::XLAAllocation alloc;
  *alloc.mutable_value() = TwoElementTuple();

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto value =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc.SerializeAsString());
  auto handle = ops::XRTAllocate(root, value);
  auto read_back = ops::XRTReadLiteralAndRelease(root, handle);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralProtos(alloc.value(), response));
}

TEST(RawApiTest, SubBuffer) {
  xrt::XLAAllocation alloc;
  *alloc.mutable_value() = NestedTuple();

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto value =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc.SerializeAsString());
  auto base_handle = ops::XRTAllocate(root, value);
  auto index_0 = ops::Const(root.WithDevice("/device:CPU:0"), {0});
  auto index_1 = ops::Const(root.WithDevice("/device:CPU:0"), {1});
  auto index_00 = ops::Const(root.WithDevice("/device:CPU:0"), {0, 0});
  auto sub_0 = ops::XRTSubTuple(root, base_handle, index_0);
  auto sub_1 = ops::XRTSubTuple(root, base_handle, index_1);
  auto sub_00 = ops::XRTSubTupleAndRelease(
      root.WithControlDependencies(
          {sub_0.output_handle.op(), sub_1.output_handle.op()}),
      base_handle, index_00);
  auto value_0 = ops::XRTReadLiteralAndRelease(root, sub_0);
  auto value_1 = ops::XRTReadLiteralAndRelease(root, sub_1);
  auto value_00 = ops::XRTReadLiteralAndRelease(root, sub_00);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({value_0, value_1, value_00}, &outputs));

  auto base_literal = xla::Literal::CreateFromProto(alloc.value()).ValueOrDie();
  auto base_elements = base_literal.DecomposeTuple();
  auto nested_0_elements = base_elements[0].Clone().DecomposeTuple();
  xla::LiteralProto response_0;
  EXPECT_TRUE(response_0.ParseFromString(outputs[0].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralToLiteralProto(base_elements[0], response_0));
  xla::LiteralProto response_1;
  EXPECT_TRUE(response_1.ParseFromString(outputs[1].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralToLiteralProto(base_elements[1], response_1));
  xla::LiteralProto response_00;
  EXPECT_TRUE(response_00.ParseFromString(outputs[2].scalar<string>()()));
  EXPECT_TRUE(CompareLiteralToLiteralProto(nested_0_elements[0], response_00));
}

TEST(RawApiTest, MakeTuple) {
  xrt::XLAAllocation alloc_0;
  *alloc_0.mutable_value() = TwoElementTuple();
  xrt::XLAAllocation alloc_1;
  *alloc_1.mutable_value() = ScalarLiteral();

  // The trivial tuple that just forwards its input and releases it.
  xrt::XLATupleNode desc_0;
  desc_0.set_input_index(0);
  desc_0.set_release_input_handle(true);

  xrt::XLATupleNode desc_1;
  auto subdesc_10 = desc_1.add_tuples();
  auto subdesc_11 = desc_1.add_tuples();
  subdesc_10->set_input_index(0);
  auto subdesc_110 = subdesc_11->add_tuples();
  subdesc_110->set_input_index(0);
  auto subdesc_111 = subdesc_11->add_tuples();
  subdesc_111->set_input_index(1);

  xrt::XLATupleNode desc_2;
  auto subdesc_20 = desc_2.add_tuples();
  auto subdesc_21 = desc_2.add_tuples();
  subdesc_20->set_input_index(1);
  subdesc_20->set_release_input_handle(true);
  subdesc_21->set_input_index(0);
  subdesc_21->set_release_input_handle(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto value_0 =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc_0.SerializeAsString());
  auto handle_0 = ops::XRTAllocate(root, value_0);
  auto value_1 =
      ops::Const(root.WithDevice("/device:CPU:0"), alloc_1.SerializeAsString());
  auto handle_1 = ops::XRTAllocate(root, value_1);
  auto tuple_0 =
      ops::Const(root.WithDevice("/device:CPU:0"), desc_0.SerializeAsString());
  auto handle_2 =
      ops::XRTMakeTuple(root, tuple_0, {static_cast<Output>(handle_0)});
  // handle_0 has now been released.
  auto tuple_1 =
      ops::Const(root.WithDevice("/device:CPU:0"), desc_1.SerializeAsString());
  auto handle_3 = ops::XRTMakeTuple(
      root, tuple_1,
      {static_cast<Output>(handle_1), static_cast<Output>(handle_2)});
  auto tuple_2 =
      ops::Const(root.WithDevice("/device:CPU:0"), desc_2.SerializeAsString());
  // Make sure this runs after handle_3 has completed, since it will free
  // handle_1 and handle_2.
  auto handle_4 = ops::XRTMakeTuple(
      root.WithControlDependencies(handle_3), tuple_2,
      {static_cast<Output>(handle_1), static_cast<Output>(handle_2)});
  // handle_1 and handle_2 have now been released.

  auto res_0 = ops::XRTReadLiteralAndRelease(root, handle_3);
  auto res_1 = ops::XRTReadLiteralAndRelease(root, handle_4);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({res_0, res_1}, &outputs));
  xla::LiteralProto response_0;
  EXPECT_TRUE(response_0.ParseFromString(outputs[0].scalar<string>()()));
  xla::LiteralProto response_1;
  EXPECT_TRUE(response_1.ParseFromString(outputs[1].scalar<string>()()));

  auto expected_0 = MakeTuple0();
  EXPECT_TRUE(CompareLiteralProtos(response_0, expected_0));
  auto expected_1 = NestedTuple();
  EXPECT_TRUE(CompareLiteralProtos(response_1, expected_1));
}

TEST(RawApiTest, CompileAndExecute) {
  xrt::XLAAllocation p0;
  *p0.mutable_value() = FloatVector({1.0f, 2.0f});
  xrt::XLAAllocation p1;
  *p1.mutable_value() = FloatVector({8.0f, 5.0f});

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->mutable_result() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  StoreComputationSnapshot(AddAndScale(), c.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto p0_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p0.SerializeAsString());
  auto p0_handle = ops::XRTAllocate(root, p0_value);
  auto p1_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p1.SerializeAsString());
  auto p1_handle = ops::XRTAllocate(root, p1_value);
  auto result = ops::XRTExecute(root, c_handle.handle, e_config,
                                {Output(p0_handle), Output(p1_handle)});
  auto read_back = ops::XRTReadLiteralAndRelease(root, result);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back, c_handle.program_shape}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));

  auto expected = xla::LiteralUtil::CreateR1<float>({27.0f, 21.0f});
  EXPECT_TRUE(CompareLiteralToLiteralProto(expected, response));

  xla::ProgramShapeProto program_shape;
  EXPECT_TRUE(program_shape.ParseFromString(outputs[1].vec<string>()(0)));
  EXPECT_EQ(program_shape.parameters_size(), 2);
}

TEST(RawApiTest, CompileAndExecuteWithArgumentVector) {
  xrt::XLAAllocation p0;
  *p0.mutable_value() = FloatVector({1.0f, 2.0f});
  xrt::XLAAllocation p1;
  *p1.mutable_value() = FloatVector({8.0f, 5.0f});

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->mutable_result() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  StoreComputationSnapshot(AddAndScale(), c.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto p0_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p0.SerializeAsString());
  auto p0_handle = ops::XRTAllocate(root, p0_value);
  auto p1_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p1.SerializeAsString());
  auto p1_handle = ops::XRTAllocate(root, p1_value);
  auto packed_args = ops::Stack(root.WithDevice("/device:CPU:0"),
                                {Output(p0_handle), Output(p1_handle)});
  auto result =
      ops::XRTExecute(root, c_handle.handle, e_config, {Output(packed_args)});
  auto read_back = ops::XRTReadLiteralAndRelease(root, result);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back, c_handle.program_shape}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));

  auto expected = xla::LiteralUtil::CreateR1<float>({27.0f, 21.0f});
  EXPECT_TRUE(CompareLiteralToLiteralProto(expected, response));

  xla::ProgramShapeProto program_shape;
  EXPECT_TRUE(program_shape.ParseFromString(outputs[1].vec<string>()(0)));
  EXPECT_EQ(program_shape.parameters_size(), 2);
}

TEST(RawApiTest, CompileWithXlaReturnShapes) {
  xla::XlaBuilder builder("XrtXlaShapes");
  auto input_shape = xla::ShapeUtil::MakeShape(xla::BF16, {32, 3, 128, 128});
  auto kernel_shape = xla::ShapeUtil::MakeShape(xla::BF16, {3, 3, 5, 5});
  // Clear layouts to signal XLA we are ready to get whatever are coming out of
  // the compilation process.
  xla::LayoutUtil::ClearLayout(&input_shape);
  xla::LayoutUtil::ClearLayout(&kernel_shape);
  auto param_shape =
      xla::ShapeUtil::MakeTupleShape({input_shape, kernel_shape});
  auto param = xla::Parameter(&builder, 0, param_shape, "param");
  auto input = xla::GetTupleElement(param, 0);
  auto kernel = xla::GetTupleElement(param, 1);
  xla::Conv(input, kernel, {1, 1}, xla::Padding::kSame);
  TF_ASSERT_OK_AND_ASSIGN(xla::XlaComputation xla_computation, builder.Build());

  auto result_shape = xla_computation.GetProgramShape().ValueOrDie().result();
  // Clear the result shape layout to tell XLA we are accepting whatever are
  // coming out of the compilation process.
  xla::LayoutUtil::ClearLayout(&result_shape);

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() = param_shape.ToProto();
  *shapes->mutable_result() = result_shape.ToProto();
  StoreComputationSnapshot(xla_computation, c.mutable_hlo_snapshot());

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto release = ops::XRTReleaseCompilationHandle(root, c_handle.handle);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(),
                           {c_handle.program_shape}, {release}, &outputs));

  xla::ProgramShapeProto program_shape_proto;
  EXPECT_TRUE(program_shape_proto.ParseFromString(outputs[0].vec<string>()(0)));
  xla::ProgramShape program_shape(program_shape_proto);
  EXPECT_EQ(program_shape.parameters_size(), 1);

  VLOG(2) << "Param: "
          << xla::ShapeUtil::HumanStringWithLayout(program_shape.parameters(0));
  VLOG(2) << "Result: "
          << xla::ShapeUtil::HumanStringWithLayout(program_shape.result());

  xla::ProgramShape xla_program_shape =
      XlaCompiledProgramShape(xla_computation, xla::ProgramShape(*shapes));
  EXPECT_TRUE(xla::Layout::Equal().MinorToMajorOnly()(
      xla::ShapeUtil::GetSubshape(program_shape.parameters(0), {0}).layout(),
      xla::ShapeUtil::GetSubshape(xla_program_shape.parameters(0), {0})
          .layout()));
  EXPECT_TRUE(xla::Layout::Equal().MinorToMajorOnly()(
      xla::ShapeUtil::GetSubshape(program_shape.parameters(0), {1}).layout(),
      xla::ShapeUtil::GetSubshape(xla_program_shape.parameters(0), {1})
          .layout()));
  EXPECT_TRUE(xla::Layout::Equal().MinorToMajorOnly()(
      program_shape.result().layout(), xla_program_shape.result().layout()));
}

TEST(RawApiTest, DotGeneralWithLayoutTest) {
  auto layout = xla::LayoutUtil::MakeLayout({0, 1});

  xrt::XLAAllocation p0;
  *p0.mutable_value() = FloatMatrix({{1.0f, 2.0f}, {3.0f, 4.0f}}, layout);
  xrt::XLAAllocation p1;
  *p1.mutable_value() = FloatMatrix({{8.0f}, {5.0f}}, layout);

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShapeWithLayout(xla::F32, {2, 2}, {0, 1}).ToProto();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShapeWithLayout(xla::F32, {2, 1}, {0, 1}).ToProto();
  *shapes->mutable_result() =
      xla::ShapeUtil::MakeShapeWithLayout(xla::F32, {2, 1}, {0, 1}).ToProto();
  StoreComputationSnapshot(Dot(), c.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto p0_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p0.SerializeAsString());
  auto p0_handle = ops::XRTAllocate(root, p0_value);
  auto p1_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p1.SerializeAsString());
  auto p1_handle = ops::XRTAllocate(root, p1_value);
  auto result = ops::XRTExecute(root, c_handle.handle, e_config,
                                {Output(p0_handle), Output(p1_handle)});
  auto read_back = ops::XRTReadLiteralAndRelease(root, result);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));

  auto expected =
      xla::LiteralUtil::CreateR2WithLayout<float>({{18.0f}, {44.0f}}, layout);

  EXPECT_TRUE(CompareLiteralToLiteralProto(expected, response));
}

TEST(RawApiTest, CompileAndExecuteZeroArg) {
  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->mutable_result() = xla::ShapeUtil::MakeShape(xla::F32, {}).ToProto();

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(true);
  StoreComputationSnapshot(OnePlusTwo(), c.mutable_hlo_snapshot());

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto result = ops::XRTExecute(root, c_handle.handle, e_config,
                                std::initializer_list<Input>({}));
  auto read_back = ops::XRTReadLiteralAndRelease(root, result);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));

  auto expected = xla::LiteralUtil::CreateR0<float>(3.0f);
  EXPECT_TRUE(CompareLiteralToLiteralProto(expected, response));
}

TEST(RawApiTest, CompileAndExecuteReturnTuple) {
  xrt::XLAAllocation p0;
  *p0.mutable_value() = FloatVector({1.0f, 2.0f});
  xrt::XLAAllocation p1;
  *p1.mutable_value() = FloatVector({8.0f, 5.0f});

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->mutable_result() =
      xla::ShapeUtil::MakeTupleShape({xla::ShapeUtil::MakeShape(xla::F32, {2})})
          .ToProto();
  StoreComputationSnapshot(AddAndTuple(), c.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto p0_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p0.SerializeAsString());
  auto p0_handle = ops::XRTAllocate(root, p0_value);
  auto p1_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p1.SerializeAsString());
  auto p1_handle = ops::XRTAllocate(root, p1_value);
  auto result = ops::XRTExecute(root, c_handle.handle, e_config,
                                {Output(p0_handle), Output(p1_handle)});
  auto read_back = ops::XRTReadLiteralAndRelease(root, result);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));

  auto sum = xla::LiteralUtil::CreateR1<float>({9.0f, 7.0f});
  auto expected = xla::LiteralUtil::MakeTuple({&sum});
  EXPECT_TRUE(CompareLiteralToLiteralProto(expected, response));
}

TEST(RawApiTest, CompileAndExecuteReturnExplodedTuple) {
  xrt::XLAAllocation p0;
  *p0.mutable_value() = xla::LiteralUtil::CreateR0<float>(12.0f).ToProto();

  xrt::XLAAllocation p1;
  *p1.mutable_value() = xla::LiteralUtil::CreateR0<float>(3.0f).ToProto();

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() = xla::ShapeUtil::MakeShape(xla::F32, {}).ToProto();
  *shapes->add_parameters() = xla::ShapeUtil::MakeShape(xla::F32, {}).ToProto();
  *shapes->mutable_result() =
      xla::ShapeUtil::MakeTupleShape({xla::ShapeUtil::MakeShape(xla::F32, {}),
                                      xla::ShapeUtil::MakeShape(xla::F32, {})})
          .ToProto();
  StoreComputationSnapshot(AddAndSubTuple(), c.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(true);
  e.set_return_exploded_tuple(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto p0_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p0.SerializeAsString());
  auto p0_handle = ops::XRTAllocate(root, p0_value);
  auto p1_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p1.SerializeAsString());
  auto p1_handle = ops::XRTAllocate(root, p1_value);
  auto result = ops::XRTExecute(root, c_handle.handle, e_config,
                                {Output(p0_handle), Output(p1_handle)});
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({result}, &outputs));
  EXPECT_EQ(outputs.size(), 1);

  auto handles_vec = outputs.front().vec<int64>();
  EXPECT_EQ(handles_vec.size(), 2);

  const float kResults[2] = {15.0f, 9.0f};
  for (int64 i = 0; i < handles_vec.size(); ++i) {
    auto read_back = ops::XRTReadLiteralAndRelease(root, Input(handles_vec(i)));
    std::vector<Tensor> voutputs;
    TF_EXPECT_OK(session.Run({read_back}, &voutputs));
    EXPECT_EQ(voutputs.size(), 1);

    xla::LiteralProto response;
    EXPECT_TRUE(response.ParseFromString(voutputs[0].scalar<string>()()));

    auto expected = xla::LiteralUtil::CreateR0<float>(kResults[i]);
    EXPECT_TRUE(CompareLiteralToLiteralProto(expected, response));
  }
}

TEST(RawApiTest, LeakCompilationReference) {
  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->add_parameters() =
      xla::ShapeUtil::MakeShape(xla::F32, {2}).ToProto();
  *shapes->mutable_result() =
      xla::ShapeUtil::MakeTupleShape({xla::ShapeUtil::MakeShape(xla::F32, {2})})
          .ToProto();
  StoreComputationSnapshot(AddAndTuple(), c.mutable_hlo_snapshot());

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({c_handle.handle}, &outputs));
}

TEST(RawApiTest, CompileAndExecuteWithReusedBuffers) {
  xla::Shape element_shape = xla::ShapeUtil::MakeShape(xla::F32, {2});
  xla::Shape shape =
      xla::ShapeUtil::MakeTupleShape({element_shape, element_shape});
  xla::Shape return_shape = xla::ShapeUtil::MakeTupleShape(
      {element_shape, element_shape, element_shape, element_shape});
  xla::XlaBuilder builder("ReuseBuffer");
  auto param = xla::Parameter(&builder, 0, shape, "param");
  auto p0 = xla::GetTupleElement(param, 0);
  auto p1 = xla::GetTupleElement(param, 1);
  auto add = xla::Add(p0, p1);
  auto sub = xla::Sub(p0, p1);
  xla::Tuple(&builder, {add, sub, p0, p1});

  // Flip the tuple literals in the input handle.
  builder.SetUpAlias({1}, 0, {0});
  builder.SetUpAlias({0}, 0, {1});

  auto computation = builder.Build().ValueOrDie();

  auto literal0 = xla::LiteralUtil::CreateR1<float>({1.0f, 2.0f});
  auto literal1 = xla::LiteralUtil::CreateR1<float>({5.0f, 9.0f});
  auto literal = xla::LiteralUtil::MakeTuple({&literal0, &literal1});

  xrt::XLAAllocation param_alloc;
  *param_alloc.mutable_value() = literal.ToProto();

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() = shape.ToProto();
  *shapes->mutable_result() = return_shape.ToProto();
  StoreComputationSnapshot(computation, c.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(false);
  e.set_release_compilation_handle(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  ClientSession session(root);
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto c_data =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, c_data);
  auto param_value = ops::Const(root.WithDevice("/device:CPU:0"),
                                param_alloc.SerializeAsString());
  auto param_handle = ops::XRTAllocate(root, param_value);
  TF_ASSERT_OK(root.status());

  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({param_handle}, &outputs));

  int64 alloc_handle = outputs[0].scalar<int64>()();

  // Note that we release the result handle immediately, but since we aliased
  // the output buffers onto the input allocation ones (held in alloc_handle),
  // we can fetch the result from there.
  auto result =
      ops::XRTExecute(root, c_handle.handle, e_config, {Input(alloc_handle)});
  auto read_back = ops::XRTReadLiteral(root, result);
  auto release = ops::XRTReleaseAllocationHandle(
      root.WithControlDependencies(read_back), result);
  TF_ASSERT_OK(root.status());

  outputs.clear();
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(), {read_back},
                           {release}, &outputs));

  xla::Literal exec_literal = ReadOutputLiteral(outputs, 0);
  auto exec_literal_parts = exec_literal.DecomposeTuple();
  ASSERT_EQ(exec_literal_parts.size(), 4);

  EXPECT_TRUE(CompareLiterals(exec_literal_parts[2], literal0));
  EXPECT_TRUE(CompareLiterals(exec_literal_parts[3], literal1));

  // Now we read back the original input handle values, which at this point
  // should contain the result of the XLA computation.
  auto read_handle = ops::XRTReadLiteral(root, Input(alloc_handle));
  TF_ASSERT_OK(root.status());
  auto release_handle = ops::XRTReleaseAllocationHandle(
      root.WithControlDependencies(read_handle), Input(alloc_handle));
  TF_ASSERT_OK(root.status());

  outputs.clear();
  TF_EXPECT_OK(session.Run(tensorflow::ClientSession::FeedType(), {read_handle},
                           {release_handle}, &outputs));

  xla::Literal return_literal = ReadOutputLiteral(outputs, 0);

  auto expected_literal0 = xla::LiteralUtil::CreateR1<float>({6.0f, 11.0f});
  auto expected_literal1 = xla::LiteralUtil::CreateR1<float>({-4.0f, -7.0f});
  // The first element of the computation returned tuple would be the add
  // (expected_literal0), but since we flipped the buffers, the sub
  // (expected_literal1) should come first.
  auto expected_literal =
      xla::LiteralUtil::MakeTuple({&expected_literal1, &expected_literal0});

  EXPECT_TRUE(CompareLiterals(return_literal, expected_literal));
}

TEST(RawApiTest, CompileAndExecuteWithS64Argument) {
  xrt::XLAAllocation p0;
  *p0.mutable_value() = xla::LiteralUtil::CreateR0<int64>(11031965).ToProto();
  xrt::XLAAllocation p1;
  *p1.mutable_value() = xla::LiteralUtil::CreateR0<int64>(4091934).ToProto();

  xrt::XLAComputation c;
  auto config = c.mutable_config();
  auto shapes = config->mutable_program_shape();
  *shapes->add_parameters() = xla::ShapeUtil::MakeShape(xla::S64, {}).ToProto();
  *shapes->add_parameters() = xla::ShapeUtil::MakeShape(xla::S64, {}).ToProto();
  *shapes->mutable_result() = xla::ShapeUtil::MakeShape(xla::S64, {}).ToProto();
  StoreComputationSnapshot(AddS64(), c.mutable_hlo_snapshot());

  xrt::XRTExecutionConfig e;
  e.set_release_input_handles(true);
  e.set_release_compilation_handle(true);
  e.set_return_exploded_tuple(true);

  Scope root = Scope::NewRootScope().WithDevice(DeviceFromFlag());
  auto e_config =
      ops::Const(root.WithDevice("/device:CPU:0"), e.SerializeAsString());
  auto computation =
      ops::Const(root.WithDevice("/device:CPU:0"), c.SerializeAsString());
  auto c_handle = ops::XRTCompile(root, computation);
  auto p0_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p0.SerializeAsString());
  auto p0_handle = ops::XRTAllocate(root, p0_value);
  auto p1_value =
      ops::Const(root.WithDevice("/device:CPU:0"), p1.SerializeAsString());
  auto p1_handle = ops::XRTAllocate(root, p1_value);
  auto result = ops::XRTExecute(root, c_handle.handle, e_config,
                                {Output(p0_handle), Output(p1_handle)});
  auto read_back = ops::XRTReadLiteralAndRelease(root, result);
  TF_ASSERT_OK(root.status());

  ClientSession session(root);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({read_back, c_handle.program_shape}, &outputs));

  xla::LiteralProto response;
  EXPECT_TRUE(response.ParseFromString(outputs[0].scalar<string>()()));

  auto expected = xla::LiteralUtil::CreateR0<int64>(15123899);
  EXPECT_TRUE(CompareLiteralToLiteralProto(expected, response));

  xla::ProgramShapeProto program_shape;
  EXPECT_TRUE(program_shape.ParseFromString(outputs[1].vec<string>()(0)));
  EXPECT_EQ(program_shape.parameters_size(), 2);
  EXPECT_TRUE(xla::ShapeUtil::HasPrimitiveType(
      xla::Shape(program_shape.result()), xla::S64));
}

}  // namespace

}  // namespace tensorflow

int main(int argc, char** argv) {
  tensorflow::xla_test_device_ptr = new tensorflow::string("XLA_CPU");
  tensorflow::xla_platform_ptr = new tensorflow::string("CPU");
  std::vector<tensorflow::Flag> flag_list = {
      tensorflow::Flag("xla_test_device", tensorflow::xla_test_device_ptr,
                       "Tensorflow device type to use for test, e.g., XLA_CPU"),
      tensorflow::Flag("xla_platform", tensorflow::xla_platform_ptr,
                       "The XLA platform to select for the device"),
  };
  tensorflow::string usage = tensorflow::Flags::Usage(argv[0], flag_list);
  const bool parse_result = tensorflow::Flags::Parse(&argc, argv, flag_list);
  if (!parse_result) {
    LOG(ERROR) << "\n" << usage;
    return 2;
  }
  testing::InitGoogleTest(&argc, argv);
  if (argc > 1) {
    LOG(ERROR) << "Unknown argument " << argv[1] << "\n" << usage;
    return 2;
  }
  return RUN_ALL_TESTS();
}
