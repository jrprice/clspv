// RUN: clspv -O0 %s -S -o %t.spvasm
// RUN: FileCheck %s < %t.spvasm
// RUN: clspv -O0 %s -o %t.spv
// RUN: spirv-dis -o %t2.spvasm %t.spv
// RUN: FileCheck %s < %t2.spvasm
// RUN: spirv-val --target-env vulkan1.0 %t.spv

// CHECK: ; SPIR-V
// CHECK: ; Version: 1.0
// CHECK: ; Generator: Codeplay; 0
// CHECK: ; Bound: 12
// CHECK: ; Schema: 0
// CHECK-DAG: OpCapability Shader
// CHECK-NOT: OpCapability StorageImageReadWithoutFormat
// CHECK-DAG: OpCapability StorageImageWriteWithoutFormat
// CHECK-DAG: OpCapability VariablePointers
// CHECK: OpExtension "SPV_KHR_variable_pointers"
// CHECK: OpMemoryModel Logical GLSL450
// CHECK: OpEntryPoint GLCompute %[[FOO_ID:[a-zA-Z0-9_]*]] "foo"
// CHECK: OpExecutionMode %[[FOO_ID]] LocalSize 1 1 1
// CHECK: OpDecorate %[[ARG0_ID:[a-zA-Z0-9_]*]] DescriptorSet 0
// CHECK: OpDecorate %[[ARG0_ID]] Binding 0
// CHECK: OpDecorate %[[ARG0_ID]] NonWritable
// CHECK: OpDecorate %[[ARG1_ID:[a-zA-Z0-9_]*]] DescriptorSet 0
// CHECK: OpDecorate %[[ARG1_ID]] Binding 1
// CHECK: OpDecorate %[[ARG1_ID]] NonReadable
// CHECK: %[[FLOAT_TYPE_ID:[a-zA-Z0-9_]*]] = OpTypeFloat 32
// CHECK: %[[READ_ONLY_IMAGE_TYPE_ID:[a-zA-Z0-9_]*]] = OpTypeImage %[[FLOAT_TYPE_ID]] 2D 0 0 0 1 Unknown
// CHECK: %[[ARG0_POINTER_TYPE_ID:[a-zA-Z0-9_]*]] = OpTypePointer UniformConstant %[[READ_ONLY_IMAGE_TYPE_ID]]
// CHECK: %[[WRITE_ONLY_IMAGE_TYPE_ID:[a-zA-Z0-9_]*]] = OpTypeImage %[[FLOAT_TYPE_ID]] 2D 0 0 0 2 Unknown
// CHECK: %[[ARG1_POINTER_TYPE_ID:[a-zA-Z0-9_]*]] = OpTypePointer UniformConstant %[[WRITE_ONLY_IMAGE_TYPE_ID]]
// CHECK: %[[VOID_TYPE_ID:[a-zA-Z0-9_]*]] = OpTypeVoid
// CHECK: %[[FOO_TYPE_ID:[a-zA-Z0-9_]*]] = OpTypeFunction %[[VOID_TYPE_ID]]
// CHECK: %[[ARG0_ID]] = OpVariable %[[ARG0_POINTER_TYPE_ID]] UniformConstant
// CHECK: %[[ARG1_ID]] = OpVariable %[[ARG1_POINTER_TYPE_ID]] UniformConstant

// CHECK: %[[FOO_ID]] = OpFunction %[[VOID_TYPE_ID]] None %[[FOO_TYPE_ID]]
// CHECK: %[[LABEL_ID:[a-zA-Z0-9_]*]] = OpLabel
// CHECK: OpReturn
// CHECK: OpFunctionEnd
void kernel __attribute__((reqd_work_group_size(1, 1, 1))) foo(read_only image2d_t a, write_only image2d_t b)
{
}
