// Copyright 2017 The Clspv Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#include <cassert>
#include <cstring>

#include <unordered_set>
#include <clspv/Option.h>
#include <clspv/Passes.h>

#include <llvm/ADT/StringSwitch.h>
#include <llvm/ADT/UniqueVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "spirv/1.0/spirv.hpp"
#include "clspv/AddressSpace.h"
#include "clspv/spirv_c_strings.hpp"
#include "clspv/spirv_glsl.hpp"

#include "ArgKind.h"
#include "ConstantEmitter.h"

#include <list>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using namespace llvm;
using namespace clspv;
using namespace mdconst;

namespace {

// The value of 1/pi.  This value is from MSDN
// https://msdn.microsoft.com/en-us/library/4hwaceh6.aspx
const double kOneOverPi = 0.318309886183790671538;
const glsl::ExtInst kGlslExtInstBad = static_cast<glsl::ExtInst>(0);

const char* kCompositeConstructFunctionPrefix = "clspv.composite_construct.";

enum SPIRVOperandType {
  NUMBERID,
  LITERAL_INTEGER,
  LITERAL_STRING,
  LITERAL_FLOAT
};

struct SPIRVOperand {
  explicit SPIRVOperand(SPIRVOperandType Ty, uint32_t Num)
      : Type(Ty), LiteralNum(1, Num) {}
  explicit SPIRVOperand(SPIRVOperandType Ty, const char *Str)
      : Type(Ty), LiteralStr(Str) {}
  explicit SPIRVOperand(SPIRVOperandType Ty, StringRef Str)
      : Type(Ty), LiteralStr(Str) {}
  explicit SPIRVOperand(SPIRVOperandType Ty, ArrayRef<uint32_t> NumVec)
      : Type(Ty), LiteralNum(NumVec.begin(), NumVec.end()) {}

  SPIRVOperandType getType() { return Type; };
  uint32_t getNumID() { return LiteralNum[0]; };
  std::string getLiteralStr() { return LiteralStr; };
  ArrayRef<uint32_t> getLiteralNum() { return LiteralNum; };

  uint32_t GetNumWords() const {
    switch (Type) {
    case NUMBERID:
      return 1;
    case LITERAL_INTEGER:
    case LITERAL_FLOAT:
      return LiteralNum.size();
    case LITERAL_STRING:
      // Account for the terminating null character.
      return (LiteralStr.size() + 4) / 4;
    }
    llvm_unreachable("Unhandled case in SPIRVOperand::GetNumWords()");
  }

private:
  SPIRVOperandType Type;
  std::string LiteralStr;
  SmallVector<uint32_t, 4> LiteralNum;
};

class SPIRVOperandList {
public:
  SPIRVOperandList() {}
  SPIRVOperandList(const SPIRVOperandList& other) = delete;
  SPIRVOperandList(SPIRVOperandList&& other) {
    contents_ = std::move(other.contents_);
    other.contents_.clear();
  }
  SPIRVOperandList(ArrayRef<SPIRVOperand *> init)
      : contents_(init.begin(), init.end()) {}
  operator ArrayRef<SPIRVOperand *>() { return contents_; }
  void push_back(SPIRVOperand *op) { contents_.push_back(op); }
  void clear() { contents_.clear();}
  size_t size() const { return contents_.size(); }
  SPIRVOperand *&operator[](size_t i) { return contents_[i]; }

  const SmallVector<SPIRVOperand *, 8> &getOperands() const {
    return contents_;
  }

private:
  SmallVector<SPIRVOperand *,8> contents_;
};

SPIRVOperandList &operator<<(SPIRVOperandList &list, SPIRVOperand *elem) {
  list.push_back(elem);
  return list;
}

SPIRVOperand* MkNum(uint32_t num) {
  return new SPIRVOperand(LITERAL_INTEGER, num);
}
SPIRVOperand* MkInteger(ArrayRef<uint32_t> num_vec) {
  return new SPIRVOperand(LITERAL_INTEGER, num_vec);
}
SPIRVOperand* MkFloat(ArrayRef<uint32_t> num_vec) {
  return new SPIRVOperand(LITERAL_FLOAT, num_vec);
}
SPIRVOperand* MkId(uint32_t id) {
  return new SPIRVOperand(NUMBERID, id);
}
SPIRVOperand* MkString(StringRef str) {
  return new SPIRVOperand(LITERAL_STRING, str);
}

struct SPIRVInstruction {
  // Create an instruction with an opcode and no result ID, and with the given
  // operands.  This computes its own word count.
  explicit SPIRVInstruction(spv::Op Opc, ArrayRef<SPIRVOperand *> Ops)
      : WordCount(1), Opcode(static_cast<uint16_t>(Opc)), ResultID(0),
        Operands(Ops.begin(), Ops.end()) {
    for (auto *operand : Ops) {
      WordCount += operand->GetNumWords();
    }
  }
  // Create an instruction with an opcode and a no-zero result ID, and
  // with the given operands.  This computes its own word count.
  explicit SPIRVInstruction(spv::Op Opc, uint32_t ResID,
                            ArrayRef<SPIRVOperand *> Ops)
      : WordCount(2), Opcode(static_cast<uint16_t>(Opc)), ResultID(ResID),
        Operands(Ops.begin(), Ops.end()) {
    if (ResID == 0) {
      llvm_unreachable("Result ID of 0 was provided");
    }
    for (auto *operand : Ops) {
      WordCount += operand->GetNumWords();
    }
  }

  uint16_t getWordCount() const { return WordCount; }
  uint16_t getOpcode() const { return Opcode; }
  uint32_t getResultID() const { return ResultID; }
  ArrayRef<SPIRVOperand *> getOperands() const { return Operands; }

private:
  uint16_t WordCount;
  uint16_t Opcode;
  uint32_t ResultID;
  SmallVector<SPIRVOperand *, 4> Operands;
};

struct SPIRVProducerPass final : public ModulePass {
  typedef DenseMap<Type *, uint32_t> TypeMapType;
  typedef UniqueVector<Type *> TypeList;
  typedef DenseMap<Value *, uint32_t> ValueMapType;
  typedef UniqueVector<Value *> ValueList;
  typedef std::vector<std::pair<Value *, uint32_t>> EntryPointVecType;
  typedef std::list<SPIRVInstruction *> SPIRVInstructionList;
  // A vector of tuples, each of which is:
  // - the LLVM instruction that we will later generate SPIR-V code for
  // - where the SPIR-V instruction should be inserted
  // - the result ID of the SPIR-V instruction
  typedef std::vector<
      std::tuple<Value *, SPIRVInstructionList::iterator, uint32_t>>
      DeferredInstVecType;
  typedef DenseMap<FunctionType *, std::pair<FunctionType *, uint32_t>>
      GlobalConstFuncMapType;

  explicit SPIRVProducerPass(
      raw_pwrite_stream &out, raw_ostream &descriptor_map_out,
      ArrayRef<std::pair<unsigned, std::string>> samplerMap, bool outputAsm,
      bool outputCInitList)
      : ModulePass(ID), samplerMap(samplerMap), out(out),
        binaryTempOut(binaryTempUnderlyingVector), binaryOut(&out),
        descriptorMapOut(descriptor_map_out), outputAsm(outputAsm),
        outputCInitList(outputCInitList), patchBoundOffset(0), nextID(1),
        OpExtInstImportID(0), HasVariablePointers(false), SamplerTy(nullptr),
        WorkgroupSizeValueID(0), WorkgroupSizeVarID(0),
        NextDescriptorSetIndex(0), constant_i32_zero_id_(0) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
  }

  virtual bool runOnModule(Module &module) override;

  // output the SPIR-V header block
  void outputHeader();

  // patch the SPIR-V header block
  void patchHeader();

  uint32_t lookupType(Type *Ty) {
    if (Ty->isPointerTy() &&
        (Ty->getPointerAddressSpace() != AddressSpace::UniformConstant)) {
      auto PointeeTy = Ty->getPointerElementType();
      if (PointeeTy->isStructTy() &&
          dyn_cast<StructType>(PointeeTy)->isOpaque()) {
        Ty = PointeeTy;
      }
    }

    if (0 == TypeMap.count(Ty)) {
      Ty->print(errs());
      llvm_unreachable("\nUnhandled type!");
    }

    return TypeMap[Ty];
  }
  TypeMapType &getImageTypeMap() { return ImageTypeMap; }
  TypeList &getTypeList() { return Types; };
  ValueList &getConstantList() { return Constants; };
  ValueMapType &getValueMap() { return ValueMap; }
  ValueMapType &getAllocatedValueMap() { return AllocatedValueMap; }
  SPIRVInstructionList &getSPIRVInstList() { return SPIRVInsts; };
  ValueToValueMapTy &getArgumentGVMap() { return ArgumentGVMap; };
  ValueMapType &getArgumentGVIDMap() { return ArgumentGVIDMap; };
  EntryPointVecType &getEntryPointVec() { return EntryPointVec; };
  DeferredInstVecType &getDeferredInstVec() { return DeferredInstVec; };
  ValueList &getEntryPointInterfacesVec() { return EntryPointInterfacesVec; };
  uint32_t &getOpExtInstImportID() { return OpExtInstImportID; };
  std::vector<uint32_t> &getBuiltinDimVec() { return BuiltinDimensionVec; };
  bool hasVariablePointers() { return true; /* We use StorageBuffer everywhere */ };
  void setVariablePointers(bool Val) { HasVariablePointers = Val; };
  ArrayRef<std::pair<unsigned, std::string>> &getSamplerMap() { return samplerMap; }
  GlobalConstFuncMapType &getGlobalConstFuncTypeMap() {
    return GlobalConstFuncTypeMap;
  }
  SmallPtrSet<Value *, 16> &getGlobalConstArgSet() {
    return GlobalConstArgumentSet;
  }
  TypeList &getTypesNeedingArrayStride() {
    return TypesNeedingArrayStride;
  }

  void GenerateLLVMIRInfo(Module &M, const DataLayout &DL);
  bool FindExtInst(Module &M);
  void FindTypePerGlobalVar(GlobalVariable &GV);
  void FindTypePerFunc(Function &F);
  // Inserts |Ty| and relevant sub-types into the |Types| member, indicating that
  // |Ty| and its subtypes will need a corresponding SPIR-V type.
  void FindType(Type *Ty);
  void FindConstantPerGlobalVar(GlobalVariable &GV);
  void FindConstantPerFunc(Function &F);
  void FindConstant(Value *V);
  void GenerateExtInstImport();
  // Generates instructions for SPIR-V types corresponding to the LLVM types
  // saved in the |Types| member.  A type follows its subtypes.  IDs are
  // allocated sequentially starting with the current value of nextID, and
  // with a type following its subtypes.  Also updates nextID to just beyond
  // the last generated ID.
  void GenerateSPIRVTypes(LLVMContext& context, const DataLayout &DL);
  void GenerateSPIRVConstants();
  void GenerateModuleInfo(Module &M);
  void GenerateGlobalVar(GlobalVariable &GV);
  void GenerateWorkgroupVars();
  void GenerateSamplers(Module &M);
  void GenerateFuncPrologue(Function &F);
  void GenerateFuncBody(Function &F);
  void GenerateInstForArg(Function &F);
  void GenerateEntryPointInitialStores();
  spv::Op GetSPIRVCmpOpcode(CmpInst *CmpI);
  spv::Op GetSPIRVCastOpcode(Instruction &I);
  spv::Op GetSPIRVBinaryOpcode(Instruction &I);
  void GenerateInstruction(Instruction &I);
  void GenerateFuncEpilogue();
  void HandleDeferredInstruction();
  void HandleDeferredDecorations(const DataLayout& DL);
  bool is4xi8vec(Type *Ty) const;
  // Return the SPIR-V Id for 32-bit constant zero.  The constant must already
  // have been created.
  uint32_t GetI32Zero();
  spv::StorageClass GetStorageClass(unsigned AddrSpace) const;
  spv::BuiltIn GetBuiltin(StringRef globalVarName) const;
  // Returns the GLSL extended instruction enum that the given function
  // call maps to.  If none, then returns the 0 value, i.e. GLSLstd4580Bad.
  glsl::ExtInst getExtInstEnum(StringRef Name);
  // Returns the GLSL extended instruction enum indirectly used by the given
  // function.  That is, to implement the given function, we use an extended
  // instruction plus one more instruction. If none, then returns the 0 value,
  // i.e. GLSLstd4580Bad.
  glsl::ExtInst getIndirectExtInstEnum(StringRef Name);
  // Returns the single GLSL extended instruction used directly or
  // indirectly by the given function call.
  glsl::ExtInst getDirectOrIndirectExtInstEnum(StringRef Name);
  void PrintResID(SPIRVInstruction *Inst);
  void PrintOpcode(SPIRVInstruction *Inst);
  void PrintOperand(SPIRVOperand *Op);
  void PrintCapability(SPIRVOperand *Op);
  void PrintExtInst(SPIRVOperand *Op);
  void PrintAddrModel(SPIRVOperand *Op);
  void PrintMemModel(SPIRVOperand *Op);
  void PrintExecModel(SPIRVOperand *Op);
  void PrintExecMode(SPIRVOperand *Op);
  void PrintSourceLanguage(SPIRVOperand *Op);
  void PrintFuncCtrl(SPIRVOperand *Op);
  void PrintStorageClass(SPIRVOperand *Op);
  void PrintDecoration(SPIRVOperand *Op);
  void PrintBuiltIn(SPIRVOperand *Op);
  void PrintSelectionControl(SPIRVOperand *Op);
  void PrintLoopControl(SPIRVOperand *Op);
  void PrintDimensionality(SPIRVOperand *Op);
  void PrintImageFormat(SPIRVOperand *Op);
  void PrintMemoryAccess(SPIRVOperand *Op);
  void PrintImageOperandsType(SPIRVOperand *Op);
  void WriteSPIRVAssembly();
  void WriteOneWord(uint32_t Word);
  void WriteResultID(SPIRVInstruction *Inst);
  void WriteWordCountAndOpcode(SPIRVInstruction *Inst);
  void WriteOperand(SPIRVOperand *Op);
  void WriteSPIRVBinary();

private:
  static char ID;
  ArrayRef<std::pair<unsigned, std::string>> samplerMap;
  raw_pwrite_stream &out;

  // TODO(dneto): Wouldn't it be better to always just emit a binary, and then
  // convert to other formats on demand?

  // When emitting a C initialization list, the WriteSPIRVBinary method
  // will actually write its words to this vector via binaryTempOut.
  SmallVector<char, 100> binaryTempUnderlyingVector;
  raw_svector_ostream binaryTempOut;

  // Binary output writes to this stream, which might be |out| or
  // |binaryTempOut|.  It's the latter when we really want to write a C
  // initializer list.
  raw_pwrite_stream* binaryOut;
  raw_ostream &descriptorMapOut;
  const bool outputAsm;
  const bool outputCInitList; // If true, output look like {0x7023, ... , 5}
  uint64_t patchBoundOffset;
  uint32_t nextID;

  // Maps an LLVM Value pointer to the corresponding SPIR-V Id.
  TypeMapType TypeMap;
  // Maps an LLVM image type to its SPIR-V ID.
  TypeMapType ImageTypeMap;
  // A unique-vector of LLVM types that map to a SPIR-V type.
  TypeList Types;
  ValueList Constants;
  // Maps an LLVM Value pointer to the corresponding SPIR-V Id.
  ValueMapType ValueMap;
  ValueMapType AllocatedValueMap;
  SPIRVInstructionList SPIRVInsts;
  // Maps a kernel argument value to a global value.  OpenCL kernel arguments
  // have to map to resources: buffers, samplers, images, or sampled images.
  ValueToValueMapTy ArgumentGVMap;
  ValueMapType ArgumentGVIDMap;
  EntryPointVecType EntryPointVec;
  DeferredInstVecType DeferredInstVec;
  ValueList EntryPointInterfacesVec;
  uint32_t OpExtInstImportID;
  std::vector<uint32_t> BuiltinDimensionVec;
  bool HasVariablePointers;
  Type *SamplerTy;

  // If a function F has a pointer-to-__constant parameter, then this variable
  // will map F's type to (G, index of the parameter), where in a first phase
  // G is F's type.  During FindTypePerFunc, G will be changed to F's type
  // but replacing the pointer-to-constant parameter with
  // pointer-to-ModuleScopePrivate.
  // TODO(dneto): This doesn't seem general enough?  A function might have
  // more than one such parameter.
  GlobalConstFuncMapType GlobalConstFuncTypeMap;
  SmallPtrSet<Value *, 16> GlobalConstArgumentSet;
  // An ordered set of pointer types of Base arguments to OpPtrAccessChain,
  // or array types, and which point into transparent memory (StorageBuffer
  // storage class).  These will require an ArrayStride decoration.
  // See SPV_KHR_variable_pointers rev 13.
  TypeList TypesNeedingArrayStride;

  // This is truly ugly, but works around what look like driver bugs.
  // For get_local_size, an earlier part of the flow has created a module-scope
  // variable in Private address space to hold the value for the workgroup
  // size.  Its intializer is a uint3 value marked as builtin WorkgroupSize.
  // When this is present, save the IDs of the initializer value and variable
  // in these two variables.  We only ever do a vector load from it, and
  // when we see one of those, substitute just the value of the intializer.
  // This mimics what Glslang does, and that's what drivers are used to.
  // TODO(dneto): Remove this once drivers are fixed.
  uint32_t WorkgroupSizeValueID;
  uint32_t WorkgroupSizeVarID;

  // What module-scope variables already have had their binding information
  // emitted?
  DenseSet<Value*> GVarWithEmittedBindingInfo;

  // An ordered list of the kernel arguments of type pointer-to-local.
  using LocalArgList = SmallVector<const Argument*, 8>;
  LocalArgList LocalArgs;
  // Information about a pointer-to-local argument.
  struct LocalArgInfo {
    // The SPIR-V ID of the array variable.
    uint32_t variable_id;
    // The element type of the
    Type* elem_type;
    // The ID of the array type.
    uint32_t array_size_id;
    // The ID of the array type.
    uint32_t array_type_id;
    // The ID of the pointer to the array type.
    uint32_t ptr_array_type_id;
    // The ID of the pointer to the first element of the array.
    uint32_t first_elem_ptr_id;
    // The specialization constant ID of the array size.
    int spec_id;
  };
  // A mapping from a pointer-to-local argument value to a LocalArgInfo value.
  DenseMap<const Argument*, LocalArgInfo> LocalArgMap;

  // The next descriptor set index to use.
  uint32_t NextDescriptorSetIndex;

  // A mapping from pointer-to-local argument to a specialization constant ID
  // for that argument's array size.  This is generated from AllocatArgSpecIds.
  ArgIdMapType ArgSpecIdMap;

  // The ID of 32-bit integer zero constant.  This is only valid after
  // GenerateSPIRVConstants has run.
  uint32_t constant_i32_zero_id_;
};

char SPIRVProducerPass::ID;

}

namespace clspv {
ModulePass *
createSPIRVProducerPass(raw_pwrite_stream &out, raw_ostream &descriptor_map_out,
                        ArrayRef<std::pair<unsigned, std::string>> samplerMap,
                        bool outputAsm, bool outputCInitList) {
  return new SPIRVProducerPass(out, descriptor_map_out, samplerMap, outputAsm,
                               outputCInitList);
}
} // namespace clspv

bool SPIRVProducerPass::runOnModule(Module &module) {
  binaryOut = outputCInitList ? &binaryTempOut : &out;

  constant_i32_zero_id_ = 0; // Reset, for the benefit of validity checks.

  ArgSpecIdMap = AllocateArgSpecIds(module);

  // SPIR-V always begins with its header information
  outputHeader();

  const DataLayout &DL = module.getDataLayout();

  // Gather information from the LLVM IR that we require.
  GenerateLLVMIRInfo(module, DL);

  // If we are using a sampler map, find the type of the sampler.
  if (0 < getSamplerMap().size()) {
    auto SamplerStructTy = module.getTypeByName("opencl.sampler_t");
    if (!SamplerStructTy) {
      SamplerStructTy =
          StructType::create(module.getContext(), "opencl.sampler_t");
    }

    SamplerTy = SamplerStructTy->getPointerTo(AddressSpace::UniformConstant);

    FindType(SamplerTy);
  }

  // Collect information on global variables too.
  for (GlobalVariable &GV : module.globals()) {
    // If the GV is one of our special __spirv_* variables, remove the
    // initializer as it was only placed there to force LLVM to not throw the
    // value away.
    if (GV.getName().startswith("__spirv_")) {
      GV.setInitializer(nullptr);
    }

    // Collect types' information from global variable.
    FindTypePerGlobalVar(GV);

    // Collect constant information from global variable.
    FindConstantPerGlobalVar(GV);

    // If the variable is an input, entry points need to know about it.
    if (AddressSpace::Input == GV.getType()->getPointerAddressSpace()) {
      getEntryPointInterfacesVec().insert(&GV);
    }
  }

  // Find types related to pointer-to-local arguments.
  for (auto& arg_spec_id_pair : ArgSpecIdMap) {
    const Argument* arg = arg_spec_id_pair.first;
    FindType(arg->getType());
    FindType(arg->getType()->getPointerElementType());
  }

  // If there are extended instructions, generate OpExtInstImport.
  if (FindExtInst(module)) {
    GenerateExtInstImport();
  }

  // Generate SPIRV instructions for types.
  GenerateSPIRVTypes(module.getContext(), DL);

  // Generate SPIRV constants.
  GenerateSPIRVConstants();

  // If we have a sampler map, we might have literal samplers to generate.
  if (0 < getSamplerMap().size()) {
    GenerateSamplers(module);
  }

  // Generate SPIRV variables.
  for (GlobalVariable &GV : module.globals()) {
    GenerateGlobalVar(GV);
  }
  GenerateWorkgroupVars();

  // Generate SPIRV instructions for each function.
  for (Function &F : module) {
    if (F.isDeclaration()) {
      continue;
    }

    // Generate Function Prologue.
    GenerateFuncPrologue(F);

    // Generate SPIRV instructions for function body.
    GenerateFuncBody(F);

    // Generate Function Epilogue.
    GenerateFuncEpilogue();
  }

  HandleDeferredInstruction();
  HandleDeferredDecorations(DL);

  // Generate SPIRV module information.
  GenerateModuleInfo(module);

  if (outputAsm) {
    WriteSPIRVAssembly();
  } else {
    WriteSPIRVBinary();
  }

  // We need to patch the SPIR-V header to set bound correctly.
  patchHeader();

  if (outputCInitList) {
    bool first = true;
    std::ostringstream os;

    auto emit_word = [&os, &first](uint32_t word) {
      if (!first)
        os << ",\n";
      os << word;
      first = false;
    };

    os << "{";
    const std::string str(binaryTempOut.str());
    for (unsigned i = 0; i < str.size(); i += 4) {
      const uint32_t a = static_cast<unsigned char>(str[i]);
      const uint32_t b = static_cast<unsigned char>(str[i + 1]);
      const uint32_t c = static_cast<unsigned char>(str[i + 2]);
      const uint32_t d = static_cast<unsigned char>(str[i + 3]);
      emit_word(a | (b << 8) | (c << 16) | (d << 24));
    }
    os << "}\n";
    out << os.str();
  }

  return false;
}

void SPIRVProducerPass::outputHeader() {
  if (outputAsm) {
    // for ASM output the header goes into 5 comments at the beginning of the
    // file
    out << "; SPIR-V\n";

    // the major version number is in the 2nd highest byte
    const uint32_t major = (spv::Version >> 16) & 0xFF;

    // the minor version number is in the 2nd lowest byte
    const uint32_t minor = (spv::Version >> 8) & 0xFF;
    out << "; Version: " << major << "." << minor << "\n";

    // use Codeplay's vendor ID
    out << "; Generator: Codeplay; 0\n";

    out << "; Bound: ";

    // we record where we need to come back to and patch in the bound value
    patchBoundOffset = out.tell();

    // output one space per digit for the max size of a 32 bit unsigned integer
    // (which is the maximum ID we could possibly be using)
    for (uint32_t i = std::numeric_limits<uint32_t>::max(); 0 != i; i /= 10) {
      out << " ";
    }

    out << "\n";

    out << "; Schema: 0\n";
  } else {
    binaryOut->write(reinterpret_cast<const char *>(&spv::MagicNumber),
              sizeof(spv::MagicNumber));
    binaryOut->write(reinterpret_cast<const char *>(&spv::Version),
              sizeof(spv::Version));

    // use Codeplay's vendor ID
    const uint32_t vendor = 3 << 16;
    binaryOut->write(reinterpret_cast<const char *>(&vendor), sizeof(vendor));

    // we record where we need to come back to and patch in the bound value
    patchBoundOffset = binaryOut->tell();

    // output a bad bound for now
    binaryOut->write(reinterpret_cast<const char *>(&nextID), sizeof(nextID));

    // output the schema (reserved for use and must be 0)
    const uint32_t schema = 0;
    binaryOut->write(reinterpret_cast<const char *>(&schema), sizeof(schema));
  }
}

void SPIRVProducerPass::patchHeader() {
  if (outputAsm) {
    // get the string representation of the max bound used (nextID will be the
    // max ID used)
    auto asString = std::to_string(nextID);
    out.pwrite(asString.c_str(), asString.size(), patchBoundOffset);
  } else {
    // for a binary we just write the value of nextID over bound
    binaryOut->pwrite(reinterpret_cast<char *>(&nextID), sizeof(nextID),
                      patchBoundOffset);
  }
}

void SPIRVProducerPass::GenerateLLVMIRInfo(Module &M, const DataLayout &DL) {
  // This function generates LLVM IR for function such as global variable for
  // argument, constant and pointer type for argument access. These information
  // is artificial one because we need Vulkan SPIR-V output. This function is
  // executed ahead of FindType and FindConstant.
  ValueToValueMapTy &ArgGVMap = getArgumentGVMap();
  LLVMContext &Context = M.getContext();

  // Map for avoiding to generate struct type with same fields.
  DenseMap<Type *, Type *> ArgTyMap;

  // These function calls need a <2 x i32> as an intermediate result but not
  // the final result.
  std::unordered_set<std::string> NeedsIVec2{
      "_Z15get_image_width14ocl_image2d_ro",
      "_Z15get_image_width14ocl_image2d_wo",
      "_Z16get_image_height14ocl_image2d_ro",
      "_Z16get_image_height14ocl_image2d_wo",
  };

  // Collect global constant variables.
  {
    SmallVector<GlobalVariable *, 8> GVList;
    SmallVector<GlobalVariable *, 8> DeadGVList;
    for (GlobalVariable &GV : M.globals()) {
      if (GV.getType()->getAddressSpace() == AddressSpace::Constant) {
        if (GV.use_empty()) {
          DeadGVList.push_back(&GV);
        } else {
          GVList.push_back(&GV);
        }
      }
    }

    // Remove dead global __constant variables.
    for (auto GV : DeadGVList) {
      GV->eraseFromParent();
    }
    DeadGVList.clear();

    if (clspv::Option::ModuleConstantsInStorageBuffer()) {
      // For now, we only support a single storage buffer.
      if (GVList.size() > 0) {
        assert(GVList.size() == 1);
        const auto *GV = GVList[0];
        const size_t constants_byte_size =
            (DL.getTypeSizeInBits(GV->getInitializer()->getType())) / 8;
        const size_t kConstantMaxSize = 65536;
        if (constants_byte_size > kConstantMaxSize) {
          outs() << "Max __constant capacity of " << kConstantMaxSize
                 << " bytes exceeded: " << constants_byte_size
                 << " bytes used\n";
          llvm_unreachable("Max __constant capacity exceeded");
        }
      }
    } else {
      // Change global constant variable's address space to ModuleScopePrivate.
      auto &GlobalConstFuncTyMap = getGlobalConstFuncTypeMap();
      for (auto GV : GVList) {
        // Create new gv with ModuleScopePrivate address space.
        Type *NewGVTy = GV->getType()->getPointerElementType();
        GlobalVariable *NewGV = new GlobalVariable(
            M, NewGVTy, false, GV->getLinkage(), GV->getInitializer(), "",
            nullptr, GV->getThreadLocalMode(),
            AddressSpace::ModuleScopePrivate);
        NewGV->takeName(GV);

        const SmallVector<User *, 8> GVUsers(GV->user_begin(), GV->user_end());
        SmallVector<User *, 8> CandidateUsers;

        auto record_called_function_type_as_user =
            [&GlobalConstFuncTyMap](Value *gv, CallInst *call) {
              // Find argument index.
              unsigned index = 0;
              for (unsigned i = 0; i < call->getNumArgOperands(); i++) {
                if (gv == call->getOperand(i)) {
                  // TODO(dneto): Should we break here?
                  index = i;
                }
              }

              // Record function type with global constant.
              GlobalConstFuncTyMap[call->getFunctionType()] =
                  std::make_pair(call->getFunctionType(), index);
            };

        for (User *GVU : GVUsers) {
          if (CallInst *Call = dyn_cast<CallInst>(GVU)) {
            record_called_function_type_as_user(GV, Call);
          } else if (GetElementPtrInst *GEP =
                         dyn_cast<GetElementPtrInst>(GVU)) {
            // Check GEP users.
            for (User *GEPU : GEP->users()) {
              if (CallInst *GEPCall = dyn_cast<CallInst>(GEPU)) {
                record_called_function_type_as_user(GEP, GEPCall);
              }
            }
          }

          CandidateUsers.push_back(GVU);
        }

        for (User *U : CandidateUsers) {
          // Update users of gv with new gv.
          U->replaceUsesOfWith(GV, NewGV);
        }

        // Delete original gv.
        GV->eraseFromParent();
      }
    }
  }

  bool HasWorkGroupBuiltin = false;
  for (GlobalVariable &GV : M.globals()) {
    const spv::BuiltIn BuiltinType = GetBuiltin(GV.getName());
    if (spv::BuiltInWorkgroupSize == BuiltinType) {
      HasWorkGroupBuiltin = true;
    }
  }


  // Map kernel functions to their ordinal number in the compilation unit.
  UniqueVector<Function*> KernelOrdinal;

  // Map the global variables created for kernel args to their creation
  // order.
  UniqueVector<GlobalVariable*> KernelArgVarOrdinal;

  // For each kernel argument type, record the kernel arg global resource variables
  // generated for that type, the function in which that variable was most
  // recently used, and the binding number it took.  For reproducibility,
  // we track things by ordinal number (rather than pointer), and we use a
  // std::set rather than DenseSet since std::set maintains an ordering.
  // Each tuple is the ordinals of the kernel function, the binding number,
  // and the ordinal of the kernal-arg-var.
  //
  // This table lets us reuse module-scope StorageBuffer variables between
  // different kernels.
  DenseMap<Type *, std::set<std::tuple<unsigned, unsigned, unsigned>>>
      GVarsForType;

  for (Function &F : M) {
    // Handle kernel function first.
    if (F.isDeclaration() || F.getCallingConv() != CallingConv::SPIR_KERNEL) {
      continue;
    }
    KernelOrdinal.insert(&F);

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (I.getOpcode() == Instruction::ZExt ||
            I.getOpcode() == Instruction::SExt ||
            I.getOpcode() == Instruction::UIToFP) {
          // If there is zext with i1 type, it will be changed to OpSelect. The
          // OpSelect needs constant 0 and 1 so the constants are added here.

          auto OpTy = I.getOperand(0)->getType();

          if (OpTy->isIntegerTy(1) ||
              (OpTy->isVectorTy() &&
               OpTy->getVectorElementType()->isIntegerTy(1))) {
            if (I.getOpcode() == Instruction::ZExt) {
              APInt One(32, 1);
              FindConstant(Constant::getNullValue(I.getType()));
              FindConstant(Constant::getIntegerValue(I.getType(), One));
            } else if (I.getOpcode() == Instruction::SExt) {
              APInt MinusOne(32, UINT64_MAX, true);
              FindConstant(Constant::getNullValue(I.getType()));
              FindConstant(Constant::getIntegerValue(I.getType(), MinusOne));
            } else {
              FindConstant(ConstantFP::get(Context, APFloat(0.0f)));
              FindConstant(ConstantFP::get(Context, APFloat(1.0f)));
            }
          }
        } else if (CallInst *Call = dyn_cast<CallInst>(&I)) {
          Function *Callee = Call->getCalledFunction();

          // Handle image type specially.
          if (Callee->getName().equals(
                  "_Z11read_imagef14ocl_image2d_ro11ocl_samplerDv2_f") ||
              Callee->getName().equals(
                  "_Z11read_imagef14ocl_image3d_ro11ocl_samplerDv4_f")) {
            TypeMapType &OpImageTypeMap = getImageTypeMap();
            Type *ImageTy =
                Call->getArgOperand(0)->getType()->getPointerElementType();
            OpImageTypeMap[ImageTy] = 0;

            FindConstant(ConstantFP::get(Context, APFloat(0.0f)));
          }

          if (NeedsIVec2.find(Callee->getName()) != NeedsIVec2.end()) {
            FindType(VectorType::get(Type::getInt32Ty(Context), 2));
          }
        }
      }
    }

    if (M.getTypeByName("opencl.image2d_ro_t") ||
        M.getTypeByName("opencl.image2d_wo_t") ||
        M.getTypeByName("opencl.image3d_ro_t") ||
        M.getTypeByName("opencl.image3d_wo_t")) {
      // Assume Image type's sampled type is float type.
      FindType(Type::getFloatTy(Context));
    }

    if (const MDNode *MD =
            dyn_cast<Function>(&F)->getMetadata("reqd_work_group_size")) {
      // We generate constants if the WorkgroupSize builtin is being used.
      if (HasWorkGroupBuiltin) {
        // Collect constant information for work group size.
        FindConstant(mdconst::extract<ConstantInt>(MD->getOperand(0)));
        FindConstant(mdconst::extract<ConstantInt>(MD->getOperand(1)));
        FindConstant(mdconst::extract<ConstantInt>(MD->getOperand(2)));
      }
    }

    // Wrap up all argument types with struct type and create global variables
    // with them.
    bool HasArgUser = false;
    unsigned Idx = 0;

    for (const Argument &Arg : F.args()) {
      Type *ArgTy = Arg.getType();

      // The pointee type of the module scope variable we will make.
      Type *GVTy = nullptr;

      Type *TmpArgTy = ArgTy;

      // sampler_t and image types have pointer type of struct type with
      // opaque type as field. Extract the struct type. It will be used by
      // global variable for argument.
      bool IsSamplerType = false;
      bool IsImageType = false;
      if (PointerType *TmpArgPTy = dyn_cast<PointerType>(TmpArgTy)) {
        if (StructType *STy =
                dyn_cast<StructType>(TmpArgPTy->getElementType())) {
          if (STy->isOpaque()) {
            if (STy->getName().equals("opencl.sampler_t")) {
              IsSamplerType = true;
              TmpArgTy = STy;
            } else if (STy->getName().equals("opencl.image2d_ro_t") ||
                       STy->getName().equals("opencl.image2d_wo_t") ||
                       STy->getName().equals("opencl.image3d_ro_t") ||
                       STy->getName().equals("opencl.image3d_wo_t")) {
              IsImageType = true;
              TmpArgTy = STy;
            } else {
              llvm_unreachable("Argument has opaque type unsupported???");
            }
          }
        }
      }
      const bool IsPointerToLocal = IsLocalPtr(ArgTy);
      // Can't both be pointer-to-local and (sampler or image).
      assert(!((IsSamplerType || IsImageType) && IsPointerToLocal));

      // Determine the address space for the module-scope variable.
      unsigned AddrSpace = AddressSpace::Global;
      if (IsSamplerType || IsImageType) {
        AddrSpace = AddressSpace::UniformConstant;
      } else if (PointerType *ArgPTy = dyn_cast<PointerType>(ArgTy)) {
        AddrSpace = ArgPTy->getAddressSpace();
      } else if (clspv::Option::PodArgsInUniformBuffer()) {
        // Use a uniform buffer for POD arguments.
        AddrSpace = AddressSpace::Uniform;
      }

      // LLVM's pointer type is distinguished by address space but we need to
      // regard constant and global address space as same here. If pointer
      // type has constant address space, generate new pointer type
      // temporarily to check previous struct type for argument.
      if (PointerType *TmpArgPTy = dyn_cast<PointerType>(TmpArgTy)) {
        if (TmpArgPTy->getAddressSpace() == AddressSpace::Constant) {
          TmpArgTy = PointerType::get(TmpArgPTy->getElementType(),
                                      AddressSpace::Global);
        }
      }

      if (IsSamplerType || IsImageType) {
        GVTy = TmpArgTy;
      } else if (IsPointerToLocal) {
        assert(ArgTy == TmpArgTy);
        auto spec_id = ArgSpecIdMap[&Arg];
        assert(spec_id > 0);
        LocalArgMap[&Arg] =
            LocalArgInfo{nextID,     ArgTy->getPointerElementType(),
                         nextID + 1, nextID + 2,
                         nextID + 3, nextID + 4,
                         spec_id};
        LocalArgs.push_back(&Arg);
        nextID += 5;
      } else if (ArgTyMap.count(TmpArgTy)) {
        // If there are arguments handled previously, use its type.
        GVTy = ArgTyMap[TmpArgTy];
      } else {
        // Wrap up argument type with struct type.
        // Reuse struct types where possible.
        SmallVector<Type*,1> members{ArgTy};
        StructType *STy = StructType::get(Context, members);

        GVTy = STy;
        ArgTyMap[TmpArgTy] = STy;
      }

      if (!IsPointerToLocal) {
        // In order to build type map between llvm type and spirv id, LLVM
        // global variable is needed. It has llvm type and other instructions
        // can access it with its type.
        //
        // Reuse a global variable if it was created for a different entry
        // point.

        // Returns a new global variable for this kernel argument, and remembers
        // it in KernelArgVarOrdinal.
        auto make_gvar = [&]() {
          auto result = new GlobalVariable(
              M, GVTy, false, GlobalValue::ExternalLinkage,
              UndefValue::get(GVTy),
              F.getName() + ".arg." + std::to_string(Idx), nullptr,
              GlobalValue::ThreadLocalMode::NotThreadLocal, AddrSpace);
          KernelArgVarOrdinal.insert(result);
          return result;
        };

        // Make a new variable if there was none for this type, or if we can
        // reuse one created for a different function but not yet reused for
        // the current function, *and* the binding is the same.
        // Always make a new variable if we're forcing distinct descriptor sets.
        GlobalVariable *GV = nullptr;
        auto which_set = GVarsForType.find(GVTy);
        if (IsSamplerType || IsImageType || which_set == GVarsForType.end() ||
            clspv::Option::DistinctKernelDescriptorSets()) {
          GV = make_gvar();
        } else {
          auto &set = which_set->second;
          // Reuse a variable if it was associated with a different function.
          for (auto iter = set.begin(), end = set.end(); iter != end; ++iter) {
            const unsigned fn_ordinal = std::get<0>(*iter);
            const unsigned binding = std::get<1>(*iter);
            if (fn_ordinal != KernelOrdinal.idFor(&F) && binding == Idx) {
              GV = KernelArgVarOrdinal[std::get<2>(*iter)];
              // Remove it from the set.  We'll add it back later.
              set.erase(iter);
              break;
            }
          }
          if (!GV) {
            GV = make_gvar();
          }
        }
        assert(GV);
        GVarsForType[GVTy].insert(std::make_tuple(
            KernelOrdinal.idFor(&F), Idx, KernelArgVarOrdinal.idFor(GV)));

        // Generate type info for argument global variable.
        FindType(GV->getType());

        ArgGVMap[&Arg] = GV;

        Idx++;
      }

      // Generate pointer type of argument type for OpAccessChain of argument.
      if (!Arg.use_empty()) {
        if (!isa<PointerType>(ArgTy)) {
          auto ty = PointerType::get(ArgTy, AddrSpace);
          FindType(ty);
        }
        HasArgUser = true;
      }
    }

    if (HasArgUser) {
      // Generate constant 0 for OpAccessChain of argument.
      Type *IdxTy = Type::getInt32Ty(Context);
      FindConstant(ConstantInt::get(IdxTy, 0));
      FindType(IdxTy);
    }

    // Collect types' information from function.
    FindTypePerFunc(F);

    // Collect constant information from function.
    FindConstantPerFunc(F);
  }

  for (Function &F : M) {
    // Handle non-kernel functions.
    if (F.isDeclaration() || F.getCallingConv() == CallingConv::SPIR_KERNEL) {
      continue;
    }

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (I.getOpcode() == Instruction::ZExt ||
            I.getOpcode() == Instruction::SExt ||
            I.getOpcode() == Instruction::UIToFP) {
          // If there is zext with i1 type, it will be changed to OpSelect. The
          // OpSelect needs constant 0 and 1 so the constants are added here.

          auto OpTy = I.getOperand(0)->getType();

          if (OpTy->isIntegerTy(1) ||
              (OpTy->isVectorTy() &&
               OpTy->getVectorElementType()->isIntegerTy(1))) {
            if (I.getOpcode() == Instruction::ZExt) {
              APInt One(32, 1);
              FindConstant(Constant::getNullValue(I.getType()));
              FindConstant(Constant::getIntegerValue(I.getType(), One));
            } else if (I.getOpcode() == Instruction::SExt) {
              APInt MinusOne(32, UINT64_MAX, true);
              FindConstant(Constant::getNullValue(I.getType()));
              FindConstant(Constant::getIntegerValue(I.getType(), MinusOne));
            } else {
              FindConstant(ConstantFP::get(Context, APFloat(0.0f)));
              FindConstant(ConstantFP::get(Context, APFloat(1.0f)));
            }
          }
        } else if (CallInst *Call = dyn_cast<CallInst>(&I)) {
          Function *Callee = Call->getCalledFunction();

          // Handle image type specially.
          if (Callee->getName().equals(
                  "_Z11read_imagef14ocl_image2d_ro11ocl_samplerDv2_f") ||
              Callee->getName().equals(
                  "_Z11read_imagef14ocl_image3d_ro11ocl_samplerDv4_f")) {
            TypeMapType &OpImageTypeMap = getImageTypeMap();
            Type *ImageTy =
                Call->getArgOperand(0)->getType()->getPointerElementType();
            OpImageTypeMap[ImageTy] = 0;

            FindConstant(ConstantFP::get(Context, APFloat(0.0f)));
          }
        }
      }
    }

    if (M.getTypeByName("opencl.image2d_ro_t") ||
        M.getTypeByName("opencl.image2d_wo_t") ||
        M.getTypeByName("opencl.image3d_ro_t") ||
        M.getTypeByName("opencl.image3d_wo_t")) {
      // Assume Image type's sampled type is float type.
      FindType(Type::getFloatTy(Context));
    }

    // Collect types' information from function.
    FindTypePerFunc(F);

    // Collect constant information from function.
    FindConstantPerFunc(F);
  }
}

bool SPIRVProducerPass::FindExtInst(Module &M) {
  LLVMContext &Context = M.getContext();
  bool HasExtInst = false;

  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (CallInst *Call = dyn_cast<CallInst>(&I)) {
          Function *Callee = Call->getCalledFunction();
          // Check whether this call is for extend instructions.
          auto callee_name = Callee->getName();
          const glsl::ExtInst EInst = getExtInstEnum(callee_name);
          const glsl::ExtInst IndirectEInst =
              getIndirectExtInstEnum(callee_name);

          HasExtInst |=
              (EInst != kGlslExtInstBad) || (IndirectEInst != kGlslExtInstBad);

          if (IndirectEInst) {
            // Register extra constants if needed.

            // Registers a type and constant for computing the result of the
            // given instruction.  If the result of the instruction is a vector,
            // then make a splat vector constant with the same number of
            // elements.
            auto register_constant = [this, &I](Constant *constant) {
              FindType(constant->getType());
              FindConstant(constant);
              if (auto *vectorTy = dyn_cast<VectorType>(I.getType())) {
                // Register the splat vector of the value with the same
                // width as the result of the instruction.
                auto *vec_constant = ConstantVector::getSplat(
                    static_cast<unsigned>(vectorTy->getNumElements()),
                    constant);
                FindConstant(vec_constant);
                FindType(vec_constant->getType());
              }
            };
            switch (IndirectEInst) {
            case glsl::ExtInstFindUMsb:
              // clz needs OpExtInst and OpISub with constant 31, or splat
              // vector of 31.  Add it to the constant list here.
              register_constant(
                  ConstantInt::get(Type::getInt32Ty(Context), 31));
              break;
            case glsl::ExtInstAcos:
            case glsl::ExtInstAsin:
            case glsl::ExtInstAtan2:
              // We need 1/pi for acospi, asinpi, atan2pi.
              register_constant(
                  ConstantFP::get(Type::getFloatTy(Context), kOneOverPi));
              break;
            default:
              assert(false && "internally inconsistent");
            }
          }
        }
      }
    }
  }

  return HasExtInst;
}

void SPIRVProducerPass::FindTypePerGlobalVar(GlobalVariable &GV) {
  // Investigate global variable's type.
  FindType(GV.getType());
}

void SPIRVProducerPass::FindTypePerFunc(Function &F) {
  // Investigate function's type.
  FunctionType *FTy = F.getFunctionType();

  if (F.getCallingConv() != CallingConv::SPIR_KERNEL) {
    auto &GlobalConstFuncTyMap = getGlobalConstFuncTypeMap();
    // Handle a regular function with global constant parameters.
    if (GlobalConstFuncTyMap.count(FTy)) {
      uint32_t GVCstArgIdx = GlobalConstFuncTypeMap[FTy].second;
      SmallVector<Type *, 4> NewFuncParamTys;
      for (unsigned i = 0; i < FTy->getNumParams(); i++) {
        Type *ParamTy = FTy->getParamType(i);
        if (i == GVCstArgIdx) {
          Type *EleTy = ParamTy->getPointerElementType();
          ParamTy = PointerType::get(EleTy, AddressSpace::ModuleScopePrivate);
        }

        NewFuncParamTys.push_back(ParamTy);
      }

      FunctionType *NewFTy =
          FunctionType::get(FTy->getReturnType(), NewFuncParamTys, false);
      GlobalConstFuncTyMap[FTy] = std::make_pair(NewFTy, GVCstArgIdx);
      FTy = NewFTy;
    }

    FindType(FTy);
  } else {
    // As kernel functions do not have parameters, create new function type and
    // add it to type map.
    SmallVector<Type *, 4> NewFuncParamTys;
    FunctionType *NewFTy =
        FunctionType::get(FTy->getReturnType(), NewFuncParamTys, false);
    FindType(NewFTy);
  }

  // Investigate instructions' type in function body.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<ShuffleVectorInst>(I)) {
        for (unsigned i = 0; i < I.getNumOperands(); i++) {
          // Ignore type for mask of shuffle vector instruction.
          if (i == 2) {
            continue;
          }

          Value *Op = I.getOperand(i);
          if (!isa<MetadataAsValue>(Op)) {
            FindType(Op->getType());
          }
        }

        FindType(I.getType());
        continue;
      }

      // Work through the operands of the instruction.
      for (unsigned i = 0; i < I.getNumOperands(); i++) {
        Value *const Op = I.getOperand(i);
        // If any of the operands is a constant, find the type!
        if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) {
          FindType(Op->getType());
        }
      }

      for (Use &Op : I.operands()) {
        if (CallInst *Call = dyn_cast<CallInst>(&I)) {
          // Avoid to check call instruction's type.
          break;
        }
        if (!isa<MetadataAsValue>(&Op)) {
          FindType(Op->getType());
          continue;
        }
      }

      CallInst *Call = dyn_cast<CallInst>(&I);

      // We don't want to track the type of this call as we are going to replace
      // it.
      if (Call && ("__translate_sampler_initializer" ==
                   Call->getCalledFunction()->getName())) {
        continue;
      }

      if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        // If gep's base operand has ModuleScopePrivate address space, make gep
        // return ModuleScopePrivate address space.
        if (GEP->getPointerAddressSpace() == AddressSpace::ModuleScopePrivate) {
          // Add pointer type with private address space for global constant to
          // type list.
          Type *EleTy = I.getType()->getPointerElementType();
          Type *NewPTy =
              PointerType::get(EleTy, AddressSpace::ModuleScopePrivate);

          FindType(NewPTy);
          continue;
        }
      }

      FindType(I.getType());
    }
  }
}

void SPIRVProducerPass::FindType(Type *Ty) {
  TypeList &TyList = getTypeList();

  if (0 != TyList.idFor(Ty)) {
    return;
  }

  if (Ty->isPointerTy()) {
    auto AddrSpace = Ty->getPointerAddressSpace();
    if ((AddressSpace::Constant == AddrSpace) ||
        (AddressSpace::Global == AddrSpace)) {
      auto PointeeTy = Ty->getPointerElementType();

      if (PointeeTy->isStructTy() &&
          dyn_cast<StructType>(PointeeTy)->isOpaque()) {
        FindType(PointeeTy);
        auto ActualPointerTy =
            PointeeTy->getPointerTo(AddressSpace::UniformConstant);
        FindType(ActualPointerTy);
        return;
      }
    }
  }

  // OpTypeArray has constant and we need to support type of the constant.
  if (isa<ArrayType>(Ty)) {
    LLVMContext &Context = Ty->getContext();
    FindType(Type::getInt32Ty(Context));
  }

  for (Type *SubTy : Ty->subtypes()) {
    FindType(SubTy);
  }

  TyList.insert(Ty);
}

void SPIRVProducerPass::FindConstantPerGlobalVar(GlobalVariable &GV) {
  // If the global variable has a (non undef) initializer.
  if (GV.hasInitializer() && !isa<UndefValue>(GV.getInitializer())) {
    FindConstant(GV.getInitializer());
  }
}

void SPIRVProducerPass::FindConstantPerFunc(Function &F) {
  // Investigate constants in function body.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      CallInst *Call = dyn_cast<CallInst>(&I);

      if (Call && ("__translate_sampler_initializer" ==
                   Call->getCalledFunction()->getName())) {
        // We've handled these constants elsewhere, so skip it.
        continue;
      }

      if (isa<AllocaInst>(I)) {
        // Alloca instruction has constant for the number of element. Ignore it.
        continue;
      } else if (isa<ShuffleVectorInst>(I)) {
        for (unsigned i = 0; i < I.getNumOperands(); i++) {
          // Ignore constant for mask of shuffle vector instruction.
          if (i == 2) {
            continue;
          }

          if (isa<Constant>(I.getOperand(i)) &&
              !isa<GlobalValue>(I.getOperand(i))) {
            FindConstant(I.getOperand(i));
          }
        }

        continue;
      } else if (isa<InsertElementInst>(I)) {
        // Handle InsertElement with <4 x i8> specially.
        Type *CompositeTy = I.getOperand(0)->getType();
        if (is4xi8vec(CompositeTy)) {
          LLVMContext &Context = CompositeTy->getContext();
          if (isa<Constant>(I.getOperand(0))) {
            FindConstant(I.getOperand(0));
          }

          if (isa<Constant>(I.getOperand(1))) {
            FindConstant(I.getOperand(1));
          }

          // Add mask constant 0xFF.
          Constant *CstFF = ConstantInt::get(Type::getInt32Ty(Context), 0xFF);
          FindConstant(CstFF);

          // Add shift amount constant.
          if (ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(2))) {
            uint64_t Idx = CI->getZExtValue();
            Constant *CstShiftAmount =
                ConstantInt::get(Type::getInt32Ty(Context), Idx * 8);
            FindConstant(CstShiftAmount);
          }

          continue;
        }

        for (unsigned i = 0; i < I.getNumOperands(); i++) {
          // Ignore constant for index of InsertElement instruction.
          if (i == 2) {
            continue;
          }

          if (isa<Constant>(I.getOperand(i)) &&
              !isa<GlobalValue>(I.getOperand(i))) {
            FindConstant(I.getOperand(i));
          }
        }

        continue;
      } else if (isa<ExtractElementInst>(I)) {
        // Handle ExtractElement with <4 x i8> specially.
        Type *CompositeTy = I.getOperand(0)->getType();
        if (is4xi8vec(CompositeTy)) {
          LLVMContext &Context = CompositeTy->getContext();
          if (isa<Constant>(I.getOperand(0))) {
            FindConstant(I.getOperand(0));
          }

          // Add mask constant 0xFF.
          Constant *CstFF = ConstantInt::get(Type::getInt32Ty(Context), 0xFF);
          FindConstant(CstFF);

          // Add shift amount constant.
          if (ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(1))) {
            uint64_t Idx = CI->getZExtValue();
            Constant *CstShiftAmount =
                ConstantInt::get(Type::getInt32Ty(Context), Idx * 8);
            FindConstant(CstShiftAmount);
          } else {
            ConstantInt *Cst8 = ConstantInt::get(Type::getInt32Ty(Context), 8);
            FindConstant(Cst8);
          }

          continue;
        }

        for (unsigned i = 0; i < I.getNumOperands(); i++) {
          // Ignore constant for index of ExtractElement instruction.
          if (i == 1) {
            continue;
          }

          if (isa<Constant>(I.getOperand(i)) &&
              !isa<GlobalValue>(I.getOperand(i))) {
            FindConstant(I.getOperand(i));
          }
        }

        continue;
      } else if ((Instruction::Xor == I.getOpcode()) && I.getType()->isIntegerTy(1)) {
        // We special case for Xor where the type is i1 and one of the arguments is a constant 1 (true), this is an OpLogicalNot in SPIR-V, and we don't need the constant
        bool foundConstantTrue = false;
        for (Use &Op : I.operands()) {
          if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) {
            auto CI = cast<ConstantInt>(Op);

            if (CI->isZero() || foundConstantTrue) {
              // If we already found the true constant, we might (probably only on -O0) have an OpLogicalNot which is taking a constant argument, so discover it anyway.
              FindConstant(Op);
            } else {
              foundConstantTrue = true;
            }
          }
        }

        continue;
      } else if (isa<TruncInst>(I)) {
        // For truncation to i8 we mask against 255.
        Type *ToTy = I.getType();
        if (8u == ToTy->getPrimitiveSizeInBits()) {
          LLVMContext &Context = ToTy->getContext();
          Constant *Cst255 = ConstantInt::get(Type::getInt32Ty(Context), 0xff);
          FindConstant(Cst255);
        }
        // Fall through.
      } else if (isa<AtomicRMWInst>(I)) {
        LLVMContext &Context = I.getContext();

        FindConstant(
            ConstantInt::get(Type::getInt32Ty(Context), spv::ScopeDevice));
        FindConstant(ConstantInt::get(
            Type::getInt32Ty(Context),
            spv::MemorySemanticsUniformMemoryMask |
                spv::MemorySemanticsSequentiallyConsistentMask));
      }

      for (Use &Op : I.operands()) {
        if (isa<Constant>(Op) && !isa<GlobalValue>(Op)) {
          FindConstant(Op);
        }
      }
    }
  }
}

void SPIRVProducerPass::FindConstant(Value *V) {
  ValueList &CstList = getConstantList();

  // If V is already tracked, ignore it.
  if (0 != CstList.idFor(V)) {
    return;
  }

  Constant *Cst = cast<Constant>(V);

  // Handle constant with <4 x i8> type specially.
  Type *CstTy = Cst->getType();
  if (is4xi8vec(CstTy)) {
    if (!isa<GlobalValue>(V)) {
      CstList.insert(V);
    }
  }

  if (Cst->getNumOperands()) {
    for (User::const_op_iterator I = Cst->op_begin(), E = Cst->op_end(); I != E;
         ++I) {
      FindConstant(*I);
    }

    CstList.insert(Cst);
    return;
  } else if (const ConstantDataSequential *CDS =
                 dyn_cast<ConstantDataSequential>(Cst)) {
    // Add constants for each element to constant list.
    for (unsigned i = 0; i < CDS->getNumElements(); i++) {
      Constant *EleCst = CDS->getElementAsConstant(i);
      FindConstant(EleCst);
    }
  }

  if (!isa<GlobalValue>(V)) {
    CstList.insert(V);
  }
}

spv::StorageClass SPIRVProducerPass::GetStorageClass(unsigned AddrSpace) const {
  switch (AddrSpace) {
  default:
    llvm_unreachable("Unsupported OpenCL address space");
  case AddressSpace::Private:
    return spv::StorageClassFunction;
  case AddressSpace::Global:
  case AddressSpace::Constant:
    return spv::StorageClassStorageBuffer;
  case AddressSpace::Input:
    return spv::StorageClassInput;
  case AddressSpace::Local:
    return spv::StorageClassWorkgroup;
  case AddressSpace::UniformConstant:
    return spv::StorageClassUniformConstant;
  case AddressSpace::Uniform:
    return spv::StorageClassUniform;
  case AddressSpace::ModuleScopePrivate:
    return spv::StorageClassPrivate;
  }
}

spv::BuiltIn SPIRVProducerPass::GetBuiltin(StringRef Name) const {
  return StringSwitch<spv::BuiltIn>(Name)
      .Case("__spirv_GlobalInvocationId", spv::BuiltInGlobalInvocationId)
      .Case("__spirv_LocalInvocationId", spv::BuiltInLocalInvocationId)
      .Case("__spirv_WorkgroupSize", spv::BuiltInWorkgroupSize)
      .Case("__spirv_NumWorkgroups", spv::BuiltInNumWorkgroups)
      .Case("__spirv_WorkgroupId", spv::BuiltInWorkgroupId)
      .Default(spv::BuiltInMax);
}

void SPIRVProducerPass::GenerateExtInstImport() {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  uint32_t &ExtInstImportID = getOpExtInstImportID();

  //
  // Generate OpExtInstImport.
  //
  // Ops[0] ... Ops[n] = Name (Literal String)
  ExtInstImportID = nextID;
  SPIRVInstList.push_back(new SPIRVInstruction(spv::OpExtInstImport, nextID++,
                                               MkString("GLSL.std.450")));
}

void SPIRVProducerPass::GenerateSPIRVTypes(LLVMContext& Context, const DataLayout &DL) {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();
  ValueMapType &AllocatedVMap = getAllocatedValueMap();
  ValueToValueMapTy &ArgGVMap = getArgumentGVMap();

  // Map for OpTypeRuntimeArray. If argument has pointer type, 2 spirv type
  // instructions are generated. They are OpTypePointer and OpTypeRuntimeArray.
  DenseMap<Type *, uint32_t> OpRuntimeTyMap;

  for (Type *Ty : getTypeList()) {
    // Update TypeMap with nextID for reference later.
    TypeMap[Ty] = nextID;

    switch (Ty->getTypeID()) {
    default: {
      Ty->print(errs());
      llvm_unreachable("Unsupported type???");
      break;
    }
    case Type::MetadataTyID:
    case Type::LabelTyID: {
      // Ignore these types.
      break;
    }
    case Type::PointerTyID: {
      PointerType *PTy = cast<PointerType>(Ty);
      unsigned AddrSpace = PTy->getAddressSpace();

      // For the purposes of our Vulkan SPIR-V type system, constant and global
      // are conflated.
      bool UseExistingOpTypePointer = false;
      if (AddressSpace::Constant == AddrSpace) {
        AddrSpace = AddressSpace::Global;

        // Check to see if we already created this type (for instance, if we had
        // a constant <type>* and a global <type>*, the type would be created by
        // one of these types, and shared by both).
        auto GlobalTy = PTy->getPointerElementType()->getPointerTo(AddrSpace);
        if (0 < TypeMap.count(GlobalTy)) {
          TypeMap[PTy] = TypeMap[GlobalTy];
          UseExistingOpTypePointer = true;
          break;
        }
      } else if (AddressSpace::Global == AddrSpace) {
        AddrSpace = AddressSpace::Constant;

        // Check to see if we already created this type (for instance, if we had
        // a constant <type>* and a global <type>*, the type would be created by
        // one of these types, and shared by both).
        auto ConstantTy = PTy->getPointerElementType()->getPointerTo(AddrSpace);
        if (0 < TypeMap.count(ConstantTy)) {
          TypeMap[PTy] = TypeMap[ConstantTy];
          UseExistingOpTypePointer = true;
        }
      }

      bool IsOpTypeRuntimeArray = false;
      bool HasArgUser = false;

      for (auto ArgGV : ArgGVMap) {
        auto Arg = ArgGV.first;

        Type *ArgTy = Arg->getType();
        if (ArgTy == PTy) {
          if (AddrSpace != AddressSpace::UniformConstant) {
            IsOpTypeRuntimeArray = true;
          }

          for (auto U : Arg->users()) {
            if (!isa<GetElementPtrInst>(U) || (U->getType() == PTy)) {
              HasArgUser = true;
              break;
            }
          }
        }
      }

      if ((!IsOpTypeRuntimeArray || HasArgUser) && !UseExistingOpTypePointer) {
        //
        // Generate OpTypePointer.
        //

        // OpTypePointer
        // Ops[0] = Storage Class
        // Ops[1] = Element Type ID
        SPIRVOperandList Ops;

        Ops << MkNum(GetStorageClass(AddrSpace))
            << MkId(lookupType(PTy->getElementType()));

        auto *Inst = new SPIRVInstruction(spv::OpTypePointer, nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      }

      if (IsOpTypeRuntimeArray) {
        //
        // Generate OpTypeRuntimeArray.
        //

        // OpTypeRuntimeArray
        // Ops[0] = Element Type ID
        SPIRVOperandList Ops;

        Type *EleTy = PTy->getElementType();
        Ops << MkId(lookupType(EleTy));

        uint32_t OpTypeRuntimeArrayID = nextID;
        assert(0 == OpRuntimeTyMap.count(Ty));
        OpRuntimeTyMap[Ty] = nextID;

        auto *Inst =
            new SPIRVInstruction(spv::OpTypeRuntimeArray, nextID++, Ops);
        SPIRVInstList.push_back(Inst);

        // Generate OpDecorate.
        auto DecoInsertPoint =
            std::find_if(SPIRVInstList.begin(), SPIRVInstList.end(),
                         [](SPIRVInstruction *Inst) -> bool {
                           return Inst->getOpcode() != spv::OpDecorate &&
                                  Inst->getOpcode() != spv::OpMemberDecorate &&
                                  Inst->getOpcode() != spv::OpExtInstImport;
                         });

        // Ops[0] = Target ID
        // Ops[1] = Decoration (ArrayStride)
        // Ops[2] = Stride Number(Literal Number)
        Ops.clear();

        Ops << MkId(OpTypeRuntimeArrayID) << MkNum(spv::DecorationArrayStride)
            << MkNum(static_cast<uint32_t>(DL.getTypeAllocSize(EleTy)));

        auto *DecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
        SPIRVInstList.insert(DecoInsertPoint, DecoInst);
      }
      break;
    }
    case Type::StructTyID: {
      LLVMContext &Context = Ty->getContext();

      StructType *STy = cast<StructType>(Ty);

      // Handle sampler type.
      if (STy->isOpaque()) {
        if (STy->getName().equals("opencl.sampler_t")) {
          //
          // Generate OpTypeSampler
          //
          // Empty Ops.
          SPIRVOperandList Ops;

          auto *Inst = new SPIRVInstruction(spv::OpTypeSampler, nextID++, Ops);
          SPIRVInstList.push_back(Inst);
          break;
        } else if (STy->getName().equals("opencl.image2d_ro_t") ||
                   STy->getName().equals("opencl.image2d_wo_t") ||
                   STy->getName().equals("opencl.image3d_ro_t") ||
                   STy->getName().equals("opencl.image3d_wo_t")) {
          //
          // Generate OpTypeImage
          //
          // Ops[0] = Sampled Type ID
          // Ops[1] = Dim ID
          // Ops[2] = Depth (Literal Number)
          // Ops[3] = Arrayed (Literal Number)
          // Ops[4] = MS (Literal Number)
          // Ops[5] = Sampled (Literal Number)
          // Ops[6] = Image Format ID
          //
          SPIRVOperandList Ops;

          // TODO: Changed Sampled Type according to situations.
          uint32_t SampledTyID = lookupType(Type::getFloatTy(Context));
          Ops << MkId(SampledTyID);

          spv::Dim DimID = spv::Dim2D;
          if (STy->getName().equals("opencl.image3d_ro_t") ||
              STy->getName().equals("opencl.image3d_wo_t")) {
            DimID = spv::Dim3D;
          }
          Ops << MkNum(DimID);

          // TODO: Set up Depth.
          Ops << MkNum(0);

          // TODO: Set up Arrayed.
          Ops << MkNum(0);

          // TODO: Set up MS.
          Ops << MkNum(0);

          // TODO: Set up Sampled.
          //
          // From Spec
          //
          // 0 indicates this is only known at run time, not at compile time
          // 1 indicates will be used with sampler
          // 2 indicates will be used without a sampler (a storage image)
          uint32_t Sampled = 1;
          if (STy->getName().equals("opencl.image2d_wo_t") ||
              STy->getName().equals("opencl.image3d_wo_t")) {
            Sampled = 2;
          }
          Ops << MkNum(Sampled);

          // TODO: Set up Image Format.
          Ops << MkNum(spv::ImageFormatUnknown);

          auto *Inst = new SPIRVInstruction(spv::OpTypeImage, nextID++, Ops);
          SPIRVInstList.push_back(Inst);
          break;
        }
      }

      //
      // Generate OpTypeStruct
      //
      // Ops[0] ... Ops[n] = Member IDs
      SPIRVOperandList Ops;

      for (auto *EleTy : STy->elements()) {
        uint32_t EleTyID = lookupType(EleTy);

        // Check OpTypeRuntimeArray.
        if (isa<PointerType>(EleTy)) {
          // TODO(dneto): Isn't this a straight lookup instead of a loop?
          for (auto ArgGV : ArgGVMap) {
            Type *ArgTy = ArgGV.first->getType();
            if (ArgTy == EleTy) {
              assert(0 != OpRuntimeTyMap.count(EleTy));
              EleTyID = OpRuntimeTyMap[EleTy];
            }
          }
        }

        Ops << MkId(EleTyID);
      }

      uint32_t STyID = nextID;

      auto *Inst =
          new SPIRVInstruction(spv::OpTypeStruct, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      // Generate OpMemberDecorate.
      auto DecoInsertPoint =
          std::find_if(SPIRVInstList.begin(), SPIRVInstList.end(),
                       [](SPIRVInstruction *Inst) -> bool {
                         return Inst->getOpcode() != spv::OpDecorate &&
                                Inst->getOpcode() != spv::OpMemberDecorate &&
                                Inst->getOpcode() != spv::OpExtInstImport;
                       });

      const auto StructLayout = DL.getStructLayout(STy);

      for (unsigned MemberIdx = 0; MemberIdx < STy->getNumElements();
           MemberIdx++) {
        // Ops[0] = Structure Type ID
        // Ops[1] = Member Index(Literal Number)
        // Ops[2] = Decoration (Offset)
        // Ops[3] = Byte Offset (Literal Number)
        Ops.clear();

        Ops << MkId(STyID) << MkNum(MemberIdx) << MkNum(spv::DecorationOffset);

        const auto ByteOffset =
            uint32_t(StructLayout->getElementOffset(MemberIdx));
        Ops << MkNum(ByteOffset);

        auto *DecoInst = new SPIRVInstruction(spv::OpMemberDecorate, Ops);
        SPIRVInstList.insert(DecoInsertPoint, DecoInst);
      }

      // Generate OpDecorate.
      for (auto ArgGV : ArgGVMap) {
        Type *ArgGVTy = ArgGV.second->getType();
        PointerType *PTy = cast<PointerType>(ArgGVTy);
        Type *ArgTy = PTy->getElementType();

        // Struct type from argument is already distinguished with the other
        // struct types on llvm types. As a result, if current processing struct
        // type is same with argument type, we can generate OpDecorate with
        // Block or BufferBlock.
        if (ArgTy == STy) {
          // Ops[0] = Target ID
          // Ops[1] = Decoration (Block or BufferBlock)
          Ops.clear();

          // Use Block decorations with StorageBuffer storage class.
          Ops << MkId(STyID) << MkNum(spv::DecorationBlock);

          auto *DecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
          SPIRVInstList.insert(DecoInsertPoint, DecoInst);
          break;
        }
      }
      break;
    }
    case Type::IntegerTyID: {
      unsigned BitWidth = Ty->getPrimitiveSizeInBits();

      if (BitWidth == 1) {
        auto *Inst = new SPIRVInstruction(spv::OpTypeBool, nextID++, {});
        SPIRVInstList.push_back(Inst);
      } else {
        // i8 is added to TypeMap as i32.
        // No matter what LLVM type is requested first, always alias the
        // second one's SPIR-V type to be the same as the one we generated
        // first.
        unsigned aliasToWidth = 0;
        if (BitWidth == 8) {
          aliasToWidth = 32;
          BitWidth = 32;
        } else if (BitWidth == 32) {
          aliasToWidth = 8;
        }
        if (aliasToWidth) {
          Type* otherType = Type::getIntNTy(Ty->getContext(), aliasToWidth);
          auto where = TypeMap.find(otherType);
          if (where == TypeMap.end()) {
            // Go ahead and make it, but also map the other type to it.
            TypeMap[otherType] = nextID;
          } else {
            // Alias this SPIR-V type the existing type.
            TypeMap[Ty] = where->second;
            break;
          }
        }

        SPIRVOperandList Ops;
        Ops << MkNum(BitWidth) << MkNum(0 /* not signed */);

        SPIRVInstList.push_back(
            new SPIRVInstruction(spv::OpTypeInt, nextID++, Ops));
      }
      break;
    }
    case Type::HalfTyID:
    case Type::FloatTyID:
    case Type::DoubleTyID: {
      SPIRVOperand *WidthOp = new SPIRVOperand(
          SPIRVOperandType::LITERAL_INTEGER, Ty->getPrimitiveSizeInBits());

      SPIRVInstList.push_back(
          new SPIRVInstruction(spv::OpTypeFloat, nextID++, WidthOp));
      break;
    }
    case Type::ArrayTyID: {
      LLVMContext &Context = Ty->getContext();
      ArrayType *ArrTy = cast<ArrayType>(Ty);
      //
      // Generate OpConstant and OpTypeArray.
      //

      //
      // Generate OpConstant for array length.
      //
      // Ops[0] = Result Type ID
      // Ops[1] .. Ops[n] = Values LiteralNumber
      SPIRVOperandList Ops;

      Type *LengthTy = Type::getInt32Ty(Context);
      uint32_t ResTyID = lookupType(LengthTy);
      Ops << MkId(ResTyID);

      uint64_t Length = ArrTy->getArrayNumElements();
      assert(Length < UINT32_MAX);
      Ops << MkNum(static_cast<uint32_t>(Length));

      // Add constant for length to constant list.
      Constant *CstLength = ConstantInt::get(LengthTy, Length);
      AllocatedVMap[CstLength] = nextID;
      VMap[CstLength] = nextID;
      uint32_t LengthID = nextID;

      auto *CstInst = new SPIRVInstruction(spv::OpConstant, nextID++, Ops);
      SPIRVInstList.push_back(CstInst);

      // Remember to generate ArrayStride later
      getTypesNeedingArrayStride().insert(Ty);

      //
      // Generate OpTypeArray.
      //
      // Ops[0] = Element Type ID
      // Ops[1] = Array Length Constant ID
      Ops.clear();

      uint32_t EleTyID = lookupType(ArrTy->getElementType());
      Ops << MkId(EleTyID) << MkId(LengthID);

      // Update TypeMap with nextID.
      TypeMap[Ty] = nextID;

      auto *ArrayInst = new SPIRVInstruction(spv::OpTypeArray, nextID++, Ops);
      SPIRVInstList.push_back(ArrayInst);
      break;
    }
    case Type::VectorTyID: {
      // <4 x i8> is changed to i32.
      LLVMContext &Context = Ty->getContext();
      if (Ty->getVectorElementType() == Type::getInt8Ty(Context)) {
        if (Ty->getVectorNumElements() == 4) {
          TypeMap[Ty] = lookupType(Ty->getVectorElementType());
          break;
        } else {
          Ty->print(errs());
          llvm_unreachable("Support above i8 vector type");
        }
      }

      // Ops[0] = Component Type ID
      // Ops[1] = Component Count (Literal Number)
      SPIRVOperandList Ops;
      Ops << MkId(lookupType(Ty->getVectorElementType()))
          << MkNum(Ty->getVectorNumElements());

      SPIRVInstruction* inst = new SPIRVInstruction(spv::OpTypeVector, nextID++, Ops);
      SPIRVInstList.push_back(inst);
      break;
    }
    case Type::VoidTyID: {
      auto *Inst = new SPIRVInstruction(spv::OpTypeVoid, nextID++, {});
      SPIRVInstList.push_back(Inst);
      break;
    }
    case Type::FunctionTyID: {
      // Generate SPIRV instruction for function type.
      FunctionType *FTy = cast<FunctionType>(Ty);

      // Ops[0] = Return Type ID
      // Ops[1] ... Ops[n] = Parameter Type IDs
      SPIRVOperandList Ops;

      // Find SPIRV instruction for return type
      Ops << MkId(lookupType(FTy->getReturnType()));

      // Find SPIRV instructions for parameter types
      for (unsigned k = 0; k < FTy->getNumParams(); k++) {
        // Find SPIRV instruction for parameter type.
        auto ParamTy = FTy->getParamType(k);
        if (ParamTy->isPointerTy()) {
          auto PointeeTy = ParamTy->getPointerElementType();
          if (PointeeTy->isStructTy() &&
              dyn_cast<StructType>(PointeeTy)->isOpaque()) {
            ParamTy = PointeeTy;
          }
        }

        Ops << MkId(lookupType(ParamTy));
      }

      auto *Inst = new SPIRVInstruction(spv::OpTypeFunction, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }
    }
  }

  // Generate OpTypeSampledImage.
  TypeMapType &OpImageTypeMap = getImageTypeMap();
  for (auto &ImageType : OpImageTypeMap) {
    //
    // Generate OpTypeSampledImage.
    //
    // Ops[0] = Image Type ID
    //
    SPIRVOperandList Ops;

    Type *ImgTy = ImageType.first;
    Ops << MkId(TypeMap[ImgTy]);

    // Update OpImageTypeMap.
    ImageType.second = nextID;

    auto *Inst = new SPIRVInstruction(spv::OpTypeSampledImage, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
  }

  // Generate types for pointer-to-local arguments.
  for (auto* arg : LocalArgs) {

    LocalArgInfo& arg_info = LocalArgMap[arg];

    // Generate the spec constant.
    SPIRVOperandList Ops;
    Ops << MkId(lookupType(Type::getInt32Ty(Context))) << MkNum(1);
    SPIRVInstList.push_back(
        new SPIRVInstruction(spv::OpSpecConstant, arg_info.array_size_id, Ops));

    // Generate the array type.
    Ops.clear();
    // The element type must have been created.
    uint32_t elem_ty_id = lookupType(arg_info.elem_type);
    assert(elem_ty_id);
    Ops << MkId(elem_ty_id) << MkId(arg_info.array_size_id);

    SPIRVInstList.push_back(
        new SPIRVInstruction(spv::OpTypeArray, arg_info.array_type_id, Ops));

    Ops.clear();
    Ops << MkNum(spv::StorageClassWorkgroup) << MkId(arg_info.array_type_id);
    SPIRVInstList.push_back(new SPIRVInstruction(
        spv::OpTypePointer, arg_info.ptr_array_type_id, Ops));
  }
}

void SPIRVProducerPass::GenerateSPIRVConstants() {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();
  ValueMapType &AllocatedVMap = getAllocatedValueMap();
  ValueList &CstList = getConstantList();
  const bool hack_undef = clspv::Option::HackUndef();

  for (uint32_t i = 0; i < CstList.size(); i++) {
    // UniqueVector ids are 1-based.
    Constant *Cst = cast<Constant>(CstList[i+1]);

    // OpTypeArray's constant was already generated.
    if (AllocatedVMap.find_as(Cst) != AllocatedVMap.end()) {
      continue;
    }

    // Set ValueMap with nextID for reference later.
    VMap[Cst] = nextID;

    //
    // Generate OpConstant.
    //

    // Ops[0] = Result Type ID
    // Ops[1] .. Ops[n] = Values LiteralNumber
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(Cst->getType()));

    std::vector<uint32_t> LiteralNum;
    spv::Op Opcode = spv::OpNop;

    if (isa<UndefValue>(Cst)) {
      // Ops[0] = Result Type ID
      Opcode = spv::OpUndef;
      if (hack_undef) {
        Type *type = Cst->getType();
        if (type->isFPOrFPVectorTy() || type->isIntOrIntVectorTy()) {
          Opcode = spv::OpConstantNull;
        }
      }
    } else if (const ConstantInt *CI = dyn_cast<ConstantInt>(Cst)) {
      unsigned BitWidth = CI->getBitWidth();
      if (BitWidth == 1) {
        // If the bitwidth of constant is 1, generate OpConstantTrue or
        // OpConstantFalse.
        if (CI->getZExtValue()) {
          // Ops[0] = Result Type ID
          Opcode = spv::OpConstantTrue;
        } else {
          // Ops[0] = Result Type ID
          Opcode = spv::OpConstantFalse;
        }
      } else {
        auto V = CI->getZExtValue();
        LiteralNum.push_back(V & 0xFFFFFFFF);

        if (BitWidth > 32) {
          LiteralNum.push_back(V >> 32);
        }

        Opcode = spv::OpConstant;

        Ops << MkInteger(LiteralNum);

        if (BitWidth == 32 && V == 0) {
          constant_i32_zero_id_ = nextID;
        }
      }
    } else if (const ConstantFP *CFP = dyn_cast<ConstantFP>(Cst)) {
      uint64_t FPVal = CFP->getValueAPF().bitcastToAPInt().getZExtValue();
      Type *CFPTy = CFP->getType();
      if (CFPTy->isFloatTy()) {
        LiteralNum.push_back(FPVal & 0xFFFFFFFF);
      } else {
        CFPTy->print(errs());
        llvm_unreachable("Implement this ConstantFP Type");
      }

      Opcode = spv::OpConstant;

      Ops << MkFloat(LiteralNum);
    } else if (isa<ConstantDataSequential>(Cst) &&
               cast<ConstantDataSequential>(Cst)->isString()) {
      Cst->print(errs());
      llvm_unreachable("Implement this Constant");

    } else if (const ConstantDataSequential *CDS =
                   dyn_cast<ConstantDataSequential>(Cst)) {
      // Let's convert <4 x i8> constant to int constant specially.
      // This case occurs when all the values are specified as constant
      // ints.
      Type *CstTy = Cst->getType();
      if (is4xi8vec(CstTy)) {
        LLVMContext &Context = CstTy->getContext();

        //
        // Generate OpConstant with OpTypeInt 32 0.
        //
        uint32_t IntValue = 0;
        for (unsigned k = 0; k < 4; k++) {
          const uint64_t Val = CDS->getElementAsInteger(k);
          IntValue = (IntValue << 8) | (Val & 0xffu);
        }

        Type *i32 = Type::getInt32Ty(Context);
        Constant *CstInt = ConstantInt::get(i32, IntValue);
        // If this constant is already registered on VMap, use it.
        if (VMap.count(CstInt)) {
          uint32_t CstID = VMap[CstInt];
          VMap[Cst] = CstID;
          continue;
        }

        Ops << MkNum(IntValue);

        auto *CstInst = new SPIRVInstruction(spv::OpConstant, nextID++, Ops);
        SPIRVInstList.push_back(CstInst);

        continue;
      }

      // A normal constant-data-sequential case.
      for (unsigned k = 0; k < CDS->getNumElements(); k++) {
        Constant *EleCst = CDS->getElementAsConstant(k);
        uint32_t EleCstID = VMap[EleCst];
        Ops << MkId(EleCstID);
      }

      Opcode = spv::OpConstantComposite;
    } else if (const ConstantAggregate *CA = dyn_cast<ConstantAggregate>(Cst)) {
      // Let's convert <4 x i8> constant to int constant specially.
      // This case occurs when at least one of the values is an undef.
      Type *CstTy = Cst->getType();
      if (is4xi8vec(CstTy)) {
        LLVMContext &Context = CstTy->getContext();

        //
        // Generate OpConstant with OpTypeInt 32 0.
        //
        uint32_t IntValue = 0;
        for (User::const_op_iterator I = Cst->op_begin(), E = Cst->op_end();
             I != E; ++I) {
          uint64_t Val = 0;
          const Value* CV = *I;
          if (auto *CI2 = dyn_cast<ConstantInt>(CV)) {
            Val = CI2->getZExtValue();
          }
          IntValue = (IntValue << 8) | (Val & 0xffu);
        }

        Type *i32 = Type::getInt32Ty(Context);
        Constant *CstInt = ConstantInt::get(i32, IntValue);
        // If this constant is already registered on VMap, use it.
        if (VMap.count(CstInt)) {
          uint32_t CstID = VMap[CstInt];
          VMap[Cst] = CstID;
          continue;
        }

        Ops << MkNum(IntValue);

        auto *CstInst = new SPIRVInstruction(spv::OpConstant, nextID++, Ops);
        SPIRVInstList.push_back(CstInst);

        continue;
      }

      // We use a constant composite in SPIR-V for our constant aggregate in
      // LLVM.
      Opcode = spv::OpConstantComposite;

      for (unsigned k = 0; k < CA->getNumOperands(); k++) {
        // Look up the ID of the element of this aggregate (which we will
        // previously have created a constant for).
        uint32_t ElementConstantID = VMap[CA->getAggregateElement(k)];

        // And add an operand to the composite we are constructing
        Ops << MkId(ElementConstantID);
      }
    } else if (Cst->isNullValue()) {
      Opcode = spv::OpConstantNull;
    } else {
      Cst->print(errs());
      llvm_unreachable("Unsupported Constant???");
    }

    auto *CstInst = new SPIRVInstruction(Opcode, nextID++, Ops);
    SPIRVInstList.push_back(CstInst);
  }
}

void SPIRVProducerPass::GenerateSamplers(Module &M) {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();

  DenseMap<unsigned, unsigned> SamplerLiteralToIDMap;

  unsigned BindingIdx = 0;

  // Generate the sampler map.
  for (auto SamplerLiteral : getSamplerMap()) {
    // Generate OpVariable.
    //
    // GIDOps[0] : Result Type ID
    // GIDOps[1] : Storage Class
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(SamplerTy))
        << MkNum(spv::StorageClassUniformConstant);

    auto *Inst = new SPIRVInstruction(spv::OpVariable, nextID, Ops);
    SPIRVInstList.push_back(Inst);

    SamplerLiteralToIDMap[SamplerLiteral.first] = nextID++;

    // Find Insert Point for OpDecorate.
    auto DecoInsertPoint =
        std::find_if(SPIRVInstList.begin(), SPIRVInstList.end(),
                     [](SPIRVInstruction *Inst) -> bool {
                       return Inst->getOpcode() != spv::OpDecorate &&
                              Inst->getOpcode() != spv::OpMemberDecorate &&
                              Inst->getOpcode() != spv::OpExtInstImport;
                     });

    // Ops[0] = Target ID
    // Ops[1] = Decoration (DescriptorSet)
    // Ops[2] = LiteralNumber according to Decoration
    Ops.clear();

    uint32_t ArgID = SamplerLiteralToIDMap[SamplerLiteral.first];
    Ops << MkId(ArgID) << MkNum(spv::DecorationDescriptorSet)
        << MkNum(NextDescriptorSetIndex);

    descriptorMapOut << "sampler," << SamplerLiteral.first << ",samplerExpr,\""
                     << SamplerLiteral.second << "\",descriptorSet,"
                     << NextDescriptorSetIndex << ",binding," << BindingIdx
                     << "\n";

    auto *DescDecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
    SPIRVInstList.insert(DecoInsertPoint, DescDecoInst);

    // Ops[0] = Target ID
    // Ops[1] = Decoration (Binding)
    // Ops[2] = LiteralNumber according to Decoration
    Ops.clear();
    Ops << MkId(ArgID) << MkNum(spv::DecorationBinding) << MkNum(BindingIdx);
    BindingIdx++;

    auto *BindDecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
    SPIRVInstList.insert(DecoInsertPoint, BindDecoInst);
  }
  if (BindingIdx > 0) {
    // We generated something.
    ++NextDescriptorSetIndex;
  }

  const char *TranslateSamplerFunctionName = "__translate_sampler_initializer";

  auto SamplerFunction = M.getFunction(TranslateSamplerFunctionName);

  // If there are no uses of the sampler function, no work to do!
  if (!SamplerFunction) {
    return;
  }

  // Iterate through the users of the sampler function.
  for (auto User : SamplerFunction->users()) {
    if (auto CI = dyn_cast<CallInst>(User)) {
      // Get the literal used to initialize the sampler.
      auto Constant = dyn_cast<ConstantInt>(CI->getArgOperand(0));

      if (!Constant) {
        CI->getArgOperand(0)->print(errs());
        llvm_unreachable("Argument of sampler initializer was non-constant!");
      }

      auto SamplerLiteral = static_cast<unsigned>(Constant->getZExtValue());

      if (0 == SamplerLiteralToIDMap.count(SamplerLiteral)) {
        Constant->print(errs());
        llvm_unreachable("Sampler literal was not found in sampler map!");
      }

      // Calls to the sampler literal function to initialize a sampler are
      // re-routed to the global variables declared for the sampler.
      VMap[CI] = SamplerLiteralToIDMap[SamplerLiteral];
    }
  }
}

void SPIRVProducerPass::GenerateGlobalVar(GlobalVariable &GV) {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();
  std::vector<uint32_t> &BuiltinDimVec = getBuiltinDimVec();
  const DataLayout &DL = GV.getParent()->getDataLayout();

  const spv::BuiltIn BuiltinType = GetBuiltin(GV.getName());
  Type *Ty = GV.getType();
  PointerType *PTy = cast<PointerType>(Ty);

  uint32_t InitializerID = 0;

  // Workgroup size is handled differently (it goes into a constant)
  if (spv::BuiltInWorkgroupSize == BuiltinType) {
    std::vector<bool> HasMDVec;
    uint32_t PrevXDimCst = 0xFFFFFFFF;
    uint32_t PrevYDimCst = 0xFFFFFFFF;
    uint32_t PrevZDimCst = 0xFFFFFFFF;
    for (Function &Func : *GV.getParent()) {
      if (Func.isDeclaration()) {
        continue;
      }

      // We only need to check kernels.
      if (Func.getCallingConv() != CallingConv::SPIR_KERNEL) {
        continue;
      }

      if (const MDNode *MD =
              dyn_cast<Function>(&Func)->getMetadata("reqd_work_group_size")) {
        uint32_t CurXDimCst = static_cast<uint32_t>(
            mdconst::extract<ConstantInt>(MD->getOperand(0))->getZExtValue());
        uint32_t CurYDimCst = static_cast<uint32_t>(
            mdconst::extract<ConstantInt>(MD->getOperand(1))->getZExtValue());
        uint32_t CurZDimCst = static_cast<uint32_t>(
            mdconst::extract<ConstantInt>(MD->getOperand(2))->getZExtValue());

        if (PrevXDimCst == 0xFFFFFFFF && PrevYDimCst == 0xFFFFFFFF &&
            PrevZDimCst == 0xFFFFFFFF) {
          PrevXDimCst = CurXDimCst;
          PrevYDimCst = CurYDimCst;
          PrevZDimCst = CurZDimCst;
        } else if (CurXDimCst != PrevXDimCst || CurYDimCst != PrevYDimCst ||
                   CurZDimCst != PrevZDimCst) {
          llvm_unreachable(
              "reqd_work_group_size must be the same across all kernels");
        } else {
          continue;
        }

        //
        // Generate OpConstantComposite.
        //
        // Ops[0] : Result Type ID
        // Ops[1] : Constant size for x dimension.
        // Ops[2] : Constant size for y dimension.
        // Ops[3] : Constant size for z dimension.
        SPIRVOperandList Ops;

        uint32_t XDimCstID =
            VMap[mdconst::extract<ConstantInt>(MD->getOperand(0))];
        uint32_t YDimCstID =
            VMap[mdconst::extract<ConstantInt>(MD->getOperand(1))];
        uint32_t ZDimCstID =
            VMap[mdconst::extract<ConstantInt>(MD->getOperand(2))];

        InitializerID = nextID;

        Ops << MkId(lookupType(Ty->getPointerElementType())) << MkId(XDimCstID)
            << MkId(YDimCstID) << MkId(ZDimCstID);

        auto *Inst =
            new SPIRVInstruction(spv::OpConstantComposite, nextID++, Ops);
        SPIRVInstList.push_back(Inst);

        HasMDVec.push_back(true);
      } else {
        HasMDVec.push_back(false);
      }
    }

    // Check all kernels have same definitions for work_group_size.
    bool HasMD = false;
    if (!HasMDVec.empty()) {
      HasMD = HasMDVec[0];
      for (uint32_t i = 1; i < HasMDVec.size(); i++) {
        if (HasMD != HasMDVec[i]) {
          llvm_unreachable(
              "Kernels should have consistent work group size definition");
        }
      }
    }

    // If all kernels do not have metadata for reqd_work_group_size, generate
    // OpSpecConstants for x/y/z dimension.
    if (!HasMD) {
      //
      // Generate OpSpecConstants for x/y/z dimension.
      //
      // Ops[0] : Result Type ID
      // Ops[1] : Constant size for x/y/z dimension (Literal Number).
      uint32_t XDimCstID = 0;
      uint32_t YDimCstID = 0;
      uint32_t ZDimCstID = 0;

      SPIRVOperandList Ops;
      uint32_t result_type_id =
          lookupType(Ty->getPointerElementType()->getSequentialElementType());

      // X Dimension
      Ops << MkId(result_type_id) << MkNum(1);
      XDimCstID = nextID++;
      SPIRVInstList.push_back(
          new SPIRVInstruction(spv::OpSpecConstant, XDimCstID, Ops));

      // Y Dimension
      Ops.clear();
      Ops << MkId(result_type_id) << MkNum(1);
      YDimCstID = nextID++;
      SPIRVInstList.push_back(
          new SPIRVInstruction(spv::OpSpecConstant, YDimCstID, Ops));

      // Z Dimension
      Ops.clear();
      Ops << MkId(result_type_id) << MkNum(1);
      ZDimCstID = nextID++;
      SPIRVInstList.push_back(
          new SPIRVInstruction(spv::OpSpecConstant, ZDimCstID, Ops));


      BuiltinDimVec.push_back(XDimCstID);
      BuiltinDimVec.push_back(YDimCstID);
      BuiltinDimVec.push_back(ZDimCstID);


      //
      // Generate OpSpecConstantComposite.
      //
      // Ops[0] : Result Type ID
      // Ops[1] : Constant size for x dimension.
      // Ops[2] : Constant size for y dimension.
      // Ops[3] : Constant size for z dimension.
      InitializerID = nextID;

      Ops.clear();
      Ops << MkId(lookupType(Ty->getPointerElementType())) << MkId(XDimCstID)
          << MkId(YDimCstID) << MkId(ZDimCstID);

      auto *Inst =
          new SPIRVInstruction(spv::OpSpecConstantComposite, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
    }
  }

  VMap[&GV] = nextID;

  //
  // Generate OpVariable.
  //
  // GIDOps[0] : Result Type ID
  // GIDOps[1] : Storage Class
  SPIRVOperandList Ops;

  const auto AS = PTy->getAddressSpace();
  Ops << MkId(lookupType(Ty)) << MkNum(GetStorageClass(AS));

  if (GV.hasInitializer()) {
    InitializerID = VMap[GV.getInitializer()];
  }

  const bool module_scope_constant_external_init =
      (0 != InitializerID) && (AS == AddressSpace::Constant) &&
      clspv::Option::ModuleConstantsInStorageBuffer();

  if (0 != InitializerID) {
    if (!module_scope_constant_external_init) {
      // Emit the ID of the intiializer as part of the variable definition.
      Ops << MkId(InitializerID);
    }
  }
  const uint32_t var_id = nextID++;

  auto *Inst = new SPIRVInstruction(spv::OpVariable, var_id, Ops);
  SPIRVInstList.push_back(Inst);

  // If we have a builtin.
  if (spv::BuiltInMax != BuiltinType) {
    // Find Insert Point for OpDecorate.
    auto DecoInsertPoint =
        std::find_if(SPIRVInstList.begin(), SPIRVInstList.end(),
                     [](SPIRVInstruction *Inst) -> bool {
                       return Inst->getOpcode() != spv::OpDecorate &&
                              Inst->getOpcode() != spv::OpMemberDecorate &&
                              Inst->getOpcode() != spv::OpExtInstImport;
                     });
    //
    // Generate OpDecorate.
    //
    // DOps[0] = Target ID
    // DOps[1] = Decoration (Builtin)
    // DOps[2] = BuiltIn ID
    uint32_t ResultID;

    // WorkgroupSize is different, we decorate the constant composite that has
    // its value, rather than the variable that we use to access the value.
    if (spv::BuiltInWorkgroupSize == BuiltinType) {
      ResultID = InitializerID;
      // Save both the value and variable IDs for later.
      WorkgroupSizeValueID = InitializerID;
      WorkgroupSizeVarID = VMap[&GV];
    } else {
      ResultID = VMap[&GV];
    }

    SPIRVOperandList DOps;
    DOps << MkId(ResultID) << MkNum(spv::DecorationBuiltIn)
         << MkNum(BuiltinType);

    auto *DescDecoInst = new SPIRVInstruction(spv::OpDecorate, DOps);
    SPIRVInstList.insert(DecoInsertPoint, DescDecoInst);
  } else if (module_scope_constant_external_init) {
    // This module scope constant is initialized from a storage buffer with data
    // provided by the host at binding 0 of the next descriptor set.
    const uint32_t descriptor_set = NextDescriptorSetIndex++;

    // Emit the intiialier to the descriptor map file.
    // Use "kind,buffer" to indicate storage buffer. We might want to expand
    // that later to other types, like uniform buffer.
    descriptorMapOut << "constant,descriptorSet," << descriptor_set
                     << ",binding,0,kind,buffer,hexbytes,";
    clspv::ConstantEmitter(DL, descriptorMapOut).Emit(GV.getInitializer());
    descriptorMapOut << "\n";

    // Find Insert Point for OpDecorate.
    auto DecoInsertPoint =
        std::find_if(SPIRVInstList.begin(), SPIRVInstList.end(),
                     [](SPIRVInstruction *Inst) -> bool {
                       return Inst->getOpcode() != spv::OpDecorate &&
                              Inst->getOpcode() != spv::OpMemberDecorate &&
                              Inst->getOpcode() != spv::OpExtInstImport;
                     });

    // OpDecorate %var Binding <binding>
    SPIRVOperandList DOps;
    DOps << MkId(var_id) << MkNum(spv::DecorationBinding) << MkNum(0);
    DecoInsertPoint = SPIRVInstList.insert(
        DecoInsertPoint, new SPIRVInstruction(spv::OpDecorate, DOps));

    // OpDecorate %var DescriptorSet <descriptor_set>
    DOps.clear();
    DOps << MkId(var_id) << MkNum(spv::DecorationDescriptorSet)
         << MkNum(descriptor_set);
    SPIRVInstList.insert(DecoInsertPoint,
                         new SPIRVInstruction(spv::OpDecorate, DOps));
  }
}

void SPIRVProducerPass::GenerateWorkgroupVars() {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  for (auto* arg : LocalArgs) {
    const auto& info = LocalArgMap[arg];

    // Generate OpVariable.
    //
    // GIDOps[0] : Result Type ID
    // GIDOps[1] : Storage Class
    SPIRVOperandList Ops;
    Ops << MkId(info.ptr_array_type_id) << MkNum(spv::StorageClassWorkgroup);

    SPIRVInstList.push_back(
        new SPIRVInstruction(spv::OpVariable, info.variable_id, Ops));
  }
}

void SPIRVProducerPass::GenerateFuncPrologue(Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();
  EntryPointVecType &EntryPoints = getEntryPointVec();
  ValueToValueMapTy &ArgGVMap = getArgumentGVMap();
  ValueMapType &ArgGVIDMap = getArgumentGVIDMap();
  auto &GlobalConstFuncTyMap = getGlobalConstFuncTypeMap();
  auto &GlobalConstArgSet = getGlobalConstArgSet();

  FunctionType *FTy = F.getFunctionType();

  //
  // Generate OpVariable and OpDecorate for kernel function with arguments.
  //
  if (F.getCallingConv() == CallingConv::SPIR_KERNEL) {

    // Find Insert Point for OpDecorate.
    auto DecoInsertPoint =
        std::find_if(SPIRVInstList.begin(), SPIRVInstList.end(),
                     [](SPIRVInstruction *Inst) -> bool {
                       return Inst->getOpcode() != spv::OpDecorate &&
                              Inst->getOpcode() != spv::OpMemberDecorate &&
                              Inst->getOpcode() != spv::OpExtInstImport;
                     });

    const uint32_t DescriptorSetIdx = NextDescriptorSetIndex;
    if (clspv::Option::DistinctKernelDescriptorSets()) {
      ++NextDescriptorSetIndex;
    }

    auto remap_arg_kind = [](StringRef argKind) {
      return clspv::Option::PodArgsInUniformBuffer() && argKind.equals("pod")
                 ? "pod_ubo"
                 : argKind;
    };

    const auto *ArgMap = F.getMetadata("kernel_arg_map");
    // Emit descriptor map entries, if there was explicit metadata
    // attached.
    if (ArgMap) {
      // The binding number is the new argument index minus the number
      // pointer-to-local arguments.  Do this adjustment here rather than
      // adding yet another data member to the metadata for each argument.
      int num_ptr_local = 0;

      for (const auto &arg : ArgMap->operands()) {
        const MDNode *arg_node = dyn_cast<MDNode>(arg.get());
        assert(arg_node->getNumOperands() == 6);
        const auto name =
            dyn_cast<MDString>(arg_node->getOperand(0))->getString();
        const auto old_index =
            dyn_extract<ConstantInt>(arg_node->getOperand(1))->getZExtValue();
        const auto new_index =
            dyn_extract<ConstantInt>(arg_node->getOperand(2))->getZExtValue();
        const auto offset =
            dyn_extract<ConstantInt>(arg_node->getOperand(3))->getZExtValue();
        const auto argKind = remap_arg_kind(
            dyn_cast<MDString>(arg_node->getOperand(4))->getString());
        const auto spec_id =
            dyn_extract<ConstantInt>(arg_node->getOperand(5))->getSExtValue();
        if (spec_id > 0) {
          num_ptr_local++;
          FunctionType *fTy =
              cast<FunctionType>(F.getType()->getPointerElementType());
          descriptorMapOut
              << "kernel," << F.getName() << ",arg," << name << ",argOrdinal,"
              << old_index << ",argKind," << argKind << ",arrayElemSize,"
              << DL.getTypeAllocSize(
                     fTy->getParamType(new_index)->getPointerElementType())
              << ",arrayNumElemSpecId," << spec_id << "\n";
        } else {
          descriptorMapOut << "kernel," << F.getName() << ",arg," << name
                           << ",argOrdinal," << old_index << ",descriptorSet,"
                           << DescriptorSetIdx << ",binding,"
                           << (new_index - num_ptr_local) << ",offset,"
                           << offset << ",argKind," << argKind << "\n";
        }
      }
    }

    uint32_t BindingIdx = 0;
    uint32_t arg_index = 0;
    for (auto &Arg : F.args()) {
      // Always use a binding, unless it's pointer-to-local.
      const bool uses_binding = !IsLocalPtr(Arg.getType());

      // Emit a descriptor map entry for this arg, in case there was no explicit
      // kernel arg mapping metadata.
      auto argKind = remap_arg_kind(clspv::GetArgKindForType(Arg.getType()));
      if (!ArgMap) {
        if (uses_binding) {
          descriptorMapOut << "kernel," << F.getName() << ",arg,"
                           << Arg.getName() << ",argOrdinal," << arg_index
                           << ",descriptorSet," << DescriptorSetIdx
                           << ",binding," << BindingIdx << ",offset,0,argKind,"
                           << argKind << "\n";
        } else {
          descriptorMapOut << "kernel," << F.getName() << ",arg,"
                           << Arg.getName() << ",argOrdinal," << arg_index
                           << ",argKind," << argKind << ",arrayElemSize,"
                           << DL.getTypeAllocSize(
                                  Arg.getType()->getPointerElementType())
                           << ",arrayNumElemSpecId," << ArgSpecIdMap[&Arg]
                           << "\n";
        }
      }

      if (uses_binding) {
        Value *NewGV = ArgGVMap[&Arg];
        VMap[&Arg] = VMap[NewGV];
        ArgGVIDMap[&Arg] = VMap[&Arg];

        if (0 == GVarWithEmittedBindingInfo.count(NewGV)) {
          // Generate a new global variable for this argument.
          GVarWithEmittedBindingInfo.insert(NewGV);

          SPIRVOperandList Ops;
          SPIRVOperand *ArgIDOp = nullptr;
          uint32_t ArgID = 0;

          if (uses_binding) {
            // Ops[0] = Target ID
            // Ops[1] = Decoration (DescriptorSet)
            // Ops[2] = LiteralNumber according to Decoration

            ArgID = VMap[&Arg];
            Ops << MkId(ArgID) << MkNum(spv::DecorationDescriptorSet)
                << MkNum(DescriptorSetIdx);

            auto *DescDecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
            SPIRVInstList.insert(DecoInsertPoint, DescDecoInst);

            // Ops[0] = Target ID
            // Ops[1] = Decoration (Binding)
            // Ops[2] = LiteralNumber according to Decoration
            Ops.clear();
            Ops << MkId(ArgID) << MkNum(spv::DecorationBinding)
                << MkNum(BindingIdx);

            auto *BindDecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
            SPIRVInstList.insert(DecoInsertPoint, BindDecoInst);
          }

          // Handle image type argument.
          bool HasReadOnlyImageType = false;
          bool HasWriteOnlyImageType = false;
          if (PointerType *ArgPTy = dyn_cast<PointerType>(Arg.getType())) {
            if (StructType *STy =
                    dyn_cast<StructType>(ArgPTy->getElementType())) {
              if (STy->isOpaque()) {
                if (STy->getName().equals("opencl.image2d_ro_t") ||
                    STy->getName().equals("opencl.image3d_ro_t")) {
                  HasReadOnlyImageType = true;
                } else if (STy->getName().equals("opencl.image2d_wo_t") ||
                           STy->getName().equals("opencl.image3d_wo_t")) {
                  HasWriteOnlyImageType = true;
                }
              }
            }
          }

          if (HasReadOnlyImageType || HasWriteOnlyImageType) {
            // Ops[0] = Target ID
            // Ops[1] = Decoration (NonReadable or NonWritable)
            Ops.clear();

            Ops << MkId(VMap[&Arg]);

            // In OpenCL 1.2 an image is either read-only or write-only, but
            // never both.
            Ops << MkNum(HasReadOnlyImageType ? spv::DecorationNonWritable
                                              : spv::DecorationNonReadable);

            auto *DescDecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
            SPIRVInstList.insert(DecoInsertPoint, DescDecoInst);
          }

          // Handle const address space.
          if (uses_binding && NewGV->getType()->getPointerAddressSpace() ==
                                  AddressSpace::Constant) {
            // Ops[0] = Target ID
            // Ops[1] = Decoration (NonWriteable)
            Ops.clear();
            assert(ArgID > 0);
            Ops << MkId(ArgID) << MkNum(spv::DecorationNonWritable);

            auto *BindDecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
            SPIRVInstList.insert(DecoInsertPoint, BindDecoInst);
          }
        }
        BindingIdx++;
      }
      arg_index++;
    }
  }

  //
  // Generate OPFunction.
  //

  // FOps[0] : Result Type ID
  // FOps[1] : Function Control
  // FOps[2] : Function Type ID
  SPIRVOperandList FOps;

  // Find SPIRV instruction for return type.
  FOps << MkId(lookupType(FTy->getReturnType()));

  // Check function attributes for SPIRV Function Control.
  uint32_t FuncControl = spv::FunctionControlMaskNone;
  if (F.hasFnAttribute(Attribute::AlwaysInline)) {
    FuncControl |= spv::FunctionControlInlineMask;
  }
  if (F.hasFnAttribute(Attribute::NoInline)) {
    FuncControl |= spv::FunctionControlDontInlineMask;
  }
  // TODO: Check llvm attribute for Function Control Pure.
  if (F.hasFnAttribute(Attribute::ReadOnly)) {
    FuncControl |= spv::FunctionControlPureMask;
  }
  // TODO: Check llvm attribute for Function Control Const.
  if (F.hasFnAttribute(Attribute::ReadNone)) {
    FuncControl |= spv::FunctionControlConstMask;
  }

  FOps << MkNum(FuncControl);

  uint32_t FTyID;
  if (F.getCallingConv() == CallingConv::SPIR_KERNEL) {
    SmallVector<Type *, 4> NewFuncParamTys;
    FunctionType *NewFTy =
        FunctionType::get(FTy->getReturnType(), NewFuncParamTys, false);
    FTyID = lookupType(NewFTy);
  } else {
    // Handle regular function with global constant parameters.
    if (GlobalConstFuncTyMap.count(FTy)) {
      FTyID = lookupType(GlobalConstFuncTyMap[FTy].first);
    } else {
      FTyID = lookupType(FTy);
    }
  }

  FOps << MkId(FTyID);

  if (F.getCallingConv() == CallingConv::SPIR_KERNEL) {
    EntryPoints.push_back(std::make_pair(&F, nextID));
  }

  VMap[&F] = nextID;

  if (clspv::Option::ShowIDs()) {
    errs() << "Function " << F.getName() << " is " << nextID << "\n";
  }
  // Generate SPIRV instruction for function.
  auto *FuncInst = new SPIRVInstruction(spv::OpFunction, nextID++, FOps);
  SPIRVInstList.push_back(FuncInst);

  //
  // Generate OpFunctionParameter for Normal function.
  //

  if (F.getCallingConv() != CallingConv::SPIR_KERNEL) {
    // Iterate Argument for name instead of param type from function type.
    unsigned ArgIdx = 0;
    for (Argument &Arg : F.args()) {
      VMap[&Arg] = nextID;

      // ParamOps[0] : Result Type ID
      SPIRVOperandList ParamOps;

      // Find SPIRV instruction for parameter type.
      uint32_t ParamTyID = lookupType(Arg.getType());
      if (PointerType *PTy = dyn_cast<PointerType>(Arg.getType())) {
        if (GlobalConstFuncTyMap.count(FTy)) {
          if (ArgIdx == GlobalConstFuncTyMap[FTy].second) {
            Type *EleTy = PTy->getPointerElementType();
            Type *ArgTy =
                PointerType::get(EleTy, AddressSpace::ModuleScopePrivate);
            ParamTyID = lookupType(ArgTy);
            GlobalConstArgSet.insert(&Arg);
          }
        }
      }
      ParamOps << MkId(ParamTyID);

      // Generate SPIRV instruction for parameter.
      auto *ParamInst =
          new SPIRVInstruction(spv::OpFunctionParameter, nextID++, ParamOps);
      SPIRVInstList.push_back(ParamInst);

      ArgIdx++;
    }
  }
}

void SPIRVProducerPass::GenerateModuleInfo(Module& module) {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  EntryPointVecType &EntryPoints = getEntryPointVec();
  ValueMapType &VMap = getValueMap();
  ValueList &EntryPointInterfaces = getEntryPointInterfacesVec();
  uint32_t &ExtInstImportID = getOpExtInstImportID();
  std::vector<uint32_t> &BuiltinDimVec = getBuiltinDimVec();

  // Set up insert point.
  auto InsertPoint = SPIRVInstList.begin();

  //
  // Generate OpCapability
  //
  // TODO: Which llvm information is mapped to SPIRV Capapbility?

  // Ops[0] = Capability
  SPIRVOperandList Ops;

  auto *CapInst =
      new SPIRVInstruction(spv::OpCapability, {MkNum(spv::CapabilityShader)});
  SPIRVInstList.insert(InsertPoint, CapInst);

  for (Type *Ty : getTypeList()) {
    // Find the i16 type.
    if (Ty->isIntegerTy(16)) {
      // Generate OpCapability for i16 type.
      SPIRVInstList.insert(InsertPoint,
                           new SPIRVInstruction(spv::OpCapability,
                                                {MkNum(spv::CapabilityInt16)}));
    } else if (Ty->isIntegerTy(64)) {
      // Generate OpCapability for i64 type.
      SPIRVInstList.insert(InsertPoint,
                           new SPIRVInstruction(spv::OpCapability,
                                                {MkNum(spv::CapabilityInt64)}));
    } else if (Ty->isHalfTy()) {
      // Generate OpCapability for half type.
      SPIRVInstList.insert(
          InsertPoint, new SPIRVInstruction(spv::OpCapability,
                                            {MkNum(spv::CapabilityFloat16)}));
    } else if (Ty->isDoubleTy()) {
      // Generate OpCapability for double type.
      SPIRVInstList.insert(
          InsertPoint, new SPIRVInstruction(spv::OpCapability,
                                            {MkNum(spv::CapabilityFloat64)}));
    } else if (auto *STy = dyn_cast<StructType>(Ty)) {
      if (STy->isOpaque()) {
        if (STy->getName().equals("opencl.image2d_wo_t") ||
            STy->getName().equals("opencl.image3d_wo_t")) {
          // Generate OpCapability for write only image type.
          SPIRVInstList.insert(
              InsertPoint,
              new SPIRVInstruction(
                  spv::OpCapability,
                  {MkNum(spv::CapabilityStorageImageWriteWithoutFormat)}));
        }
      }
    }
  }

  { // OpCapability ImageQuery
    bool hasImageQuery = false;
    for (const char *imageQuery : {
             "_Z15get_image_width14ocl_image2d_ro",
             "_Z15get_image_width14ocl_image2d_wo",
             "_Z16get_image_height14ocl_image2d_ro",
             "_Z16get_image_height14ocl_image2d_wo",
         }) {
      if (module.getFunction(imageQuery)) {
        hasImageQuery = true;
        break;
      }
    }
    if (hasImageQuery) {
      auto *ImageQueryCapInst = new SPIRVInstruction(
          spv::OpCapability, {MkNum(spv::CapabilityImageQuery)});
      SPIRVInstList.insert(InsertPoint, ImageQueryCapInst);
    }
  }

  if (hasVariablePointers()) {
    //
    // Generate OpCapability and OpExtension
    //

    //
    // Generate OpCapability.
    //
    // Ops[0] = Capability
    //
    Ops.clear();
    Ops << MkNum(spv::CapabilityVariablePointers);

    SPIRVInstList.insert(InsertPoint,
                         new SPIRVInstruction(spv::OpCapability, Ops));

    //
    // Generate OpExtension.
    //
    // Ops[0] = Name (Literal String)
    //
    for (auto extension : {"SPV_KHR_storage_buffer_storage_class",
                           "SPV_KHR_variable_pointers"}) {

      auto *ExtensionInst =
          new SPIRVInstruction(spv::OpExtension, {MkString(extension)});
      SPIRVInstList.insert(InsertPoint, ExtensionInst);
    }
  }

  if (ExtInstImportID) {
    ++InsertPoint;
  }

  //
  // Generate OpMemoryModel
  //
  // Memory model for Vulkan will always be GLSL450.

  // Ops[0] = Addressing Model
  // Ops[1] = Memory Model
  Ops.clear();
  Ops << MkNum(spv::AddressingModelLogical) << MkNum(spv::MemoryModelGLSL450);

  auto *MemModelInst = new SPIRVInstruction(spv::OpMemoryModel, Ops);
  SPIRVInstList.insert(InsertPoint, MemModelInst);

  //
  // Generate OpEntryPoint
  //
  for (auto EntryPoint : EntryPoints) {
    // Ops[0] = Execution Model
    // Ops[1] = EntryPoint ID
    // Ops[2] = Name (Literal String)
    // ...
    //
    // TODO: Do we need to consider Interface ID for forward references???
    Ops.clear();
    const StringRef& name = EntryPoint.first->getName();
    Ops << MkNum(spv::ExecutionModelGLCompute) << MkId(EntryPoint.second)
        << MkString(name);

    for (Value *Interface : EntryPointInterfaces) {
      Ops << MkId(VMap[Interface]);
    }

    auto *EntryPointInst = new SPIRVInstruction(spv::OpEntryPoint, Ops);
    SPIRVInstList.insert(InsertPoint, EntryPointInst);
  }

  for (auto EntryPoint : EntryPoints) {
    if (const MDNode *MD = dyn_cast<Function>(EntryPoint.first)
                               ->getMetadata("reqd_work_group_size")) {

      if (!BuiltinDimVec.empty()) {
        llvm_unreachable(
            "Kernels should have consistent work group size definition");
      }

      //
      // Generate OpExecutionMode
      //

      // Ops[0] = Entry Point ID
      // Ops[1] = Execution Mode
      // Ops[2] ... Ops[n] = Optional literals according to Execution Mode
      Ops.clear();
      Ops << MkId(EntryPoint.second)
          << MkNum(spv::ExecutionModeLocalSize);

      uint32_t XDim = static_cast<uint32_t>(
          mdconst::extract<ConstantInt>(MD->getOperand(0))->getZExtValue());
      uint32_t YDim = static_cast<uint32_t>(
          mdconst::extract<ConstantInt>(MD->getOperand(1))->getZExtValue());
      uint32_t ZDim = static_cast<uint32_t>(
          mdconst::extract<ConstantInt>(MD->getOperand(2))->getZExtValue());

      Ops << MkNum(XDim) << MkNum(YDim) << MkNum(ZDim);

      auto *ExecModeInst = new SPIRVInstruction(spv::OpExecutionMode, Ops);
      SPIRVInstList.insert(InsertPoint, ExecModeInst);
    }
  }

  //
  // Generate OpSource.
  //
  // Ops[0] = SourceLanguage ID
  // Ops[1] = Version (LiteralNum)
  //
  Ops.clear();
  Ops << MkNum(spv::SourceLanguageOpenCL_C) << MkNum(120);

  auto *OpenSourceInst = new SPIRVInstruction(spv::OpSource, Ops);
  SPIRVInstList.insert(InsertPoint, OpenSourceInst);

  if (!BuiltinDimVec.empty()) {
    //
    // Generate OpDecorates for x/y/z dimension.
    //
    // Ops[0] = Target ID
    // Ops[1] = Decoration (SpecId)
    // Ops[2] = Specialization Constant ID (Literal Number)

    // X Dimension
    Ops.clear();
    Ops << MkId(BuiltinDimVec[0]) << MkNum(spv::DecorationSpecId) << MkNum(0);
    SPIRVInstList.insert(InsertPoint,
                         new SPIRVInstruction(spv::OpDecorate, Ops));

    // Y Dimension
    Ops.clear();
    Ops << MkId(BuiltinDimVec[1]) << MkNum(spv::DecorationSpecId) << MkNum(1);
    SPIRVInstList.insert(InsertPoint,
                         new SPIRVInstruction(spv::OpDecorate, Ops));

    // Z Dimension
    Ops.clear();
    Ops << MkId(BuiltinDimVec[2]) << MkNum(spv::DecorationSpecId) << MkNum(2);
    SPIRVInstList.insert(InsertPoint,
                         new SPIRVInstruction(spv::OpDecorate, Ops));
  }
}

void SPIRVProducerPass::GenerateInstForArg(Function &F) {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();
  Module *Module = F.getParent();
  LLVMContext &Context = Module->getContext();
  ValueToValueMapTy &ArgGVMap = getArgumentGVMap();

  for (Argument &Arg : F.args()) {
    if (Arg.use_empty()) {
      continue;
    }

    Type *ArgTy = Arg.getType();
    if (IsLocalPtr(ArgTy)) {
      // Generate OpAccessChain to point to the first element of the array.
      const LocalArgInfo &info = LocalArgMap[&Arg];
      VMap[&Arg] = info.first_elem_ptr_id;

      SPIRVOperandList Ops;
      uint32_t zeroId = VMap[ConstantInt::get(Type::getInt32Ty(Context), 0)];
      Ops << MkId(lookupType(ArgTy)) << MkId(info.variable_id) << MkId(zeroId);
      SPIRVInstList.push_back(new SPIRVInstruction(
          spv::OpAccessChain, info.first_elem_ptr_id, Ops));

      continue;
    }

    // Check the type of users of arguments.
    bool HasOnlyGEPUse = true;
    for (auto *U : Arg.users()) {
      if (!isa<GetElementPtrInst>(U) && isa<Instruction>(U)) {
        HasOnlyGEPUse = false;
        break;
      }
    }

    if (PointerType *PTy = dyn_cast<PointerType>(ArgTy)) {
      if (StructType *STy = dyn_cast<StructType>(PTy->getElementType())) {
        if (STy->isOpaque()) {
          // Generate OpLoad for sampler and image types.
          if (STy->getName().equals("opencl.sampler_t") ||
              STy->getName().equals("opencl.image2d_ro_t") ||
              STy->getName().equals("opencl.image2d_wo_t") ||
              STy->getName().equals("opencl.image3d_ro_t") ||
              STy->getName().equals("opencl.image3d_wo_t")) {
            //
            // Generate OpLoad.
            //
            // Ops[0] = Result Type ID
            // Ops[1] = Pointer ID
            // Ops[2] ... Ops[n] = Optional Memory Access
            //
            // TODO: Do we need to implement Optional Memory Access???
            SPIRVOperandList Ops;

            // Use type with address space modified.
            ArgTy = ArgGVMap[&Arg]->getType()->getPointerElementType();

            Ops << MkId(lookupType(ArgTy));

            uint32_t PointerID = VMap[&Arg];
            Ops << MkId(PointerID);

            VMap[&Arg] = nextID;
            auto *Inst = new SPIRVInstruction(spv::OpLoad, nextID++, Ops);
            SPIRVInstList.push_back(Inst);
            continue;
          }
        }
      }

      if (!HasOnlyGEPUse) {
        //
        // Generate OpAccessChain.
        //
        // Ops[0] = Result Type ID
        // Ops[1] = Base ID
        // Ops[2] ... Ops[n] = Indexes ID
        SPIRVOperandList Ops;

        uint32_t ResTyID = lookupType(ArgTy);
        if (!isa<PointerType>(ArgTy)) {
          ResTyID = lookupType(PointerType::get(ArgTy, AddressSpace::Global));
        }
        Ops << MkId(ResTyID);

        uint32_t BaseID = VMap[&Arg];
        Ops << MkId(BaseID) << MkId(GetI32Zero())
            << MkId(GetI32Zero());

        // Generate SPIRV instruction for argument.
        VMap[&Arg] = nextID;
        auto *ArgInst = new SPIRVInstruction(spv::OpAccessChain, nextID++, Ops);
        SPIRVInstList.push_back(ArgInst);
      } else {
        // For GEP uses, generate OpAccessChain with folding GEP ahead of GEP.
        // Nothing to do here.
      }
    } else {
      //
      // Generate OpAccessChain and OpLoad for non-pointer type argument.
      //

      //
      // Generate OpAccessChain.
      //
      // Ops[0] = Result Type ID
      // Ops[1] = Base ID
      // Ops[2] ... Ops[n] = Indexes ID
      SPIRVOperandList Ops;

      uint32_t ResTyID = lookupType(ArgTy);
      if (!isa<PointerType>(ArgTy)) {
        auto AS = clspv::Option::PodArgsInUniformBuffer()
                      ? AddressSpace::Uniform
                      : AddressSpace::Global;
        ResTyID = lookupType(PointerType::get(ArgTy, AS));
      }
      Ops << MkId(ResTyID);

      uint32_t BaseID = VMap[&Arg];
      Ops << MkId(BaseID) << MkId(GetI32Zero());

      // Generate SPIRV instruction for argument.
      uint32_t PointerID = nextID;
      VMap[&Arg] = nextID;
      auto *ArgInst = new SPIRVInstruction(spv::OpAccessChain, nextID++, Ops);
      SPIRVInstList.push_back(ArgInst);

      //
      // Generate OpLoad.
      //

      // Ops[0] = Result Type ID
      // Ops[1] = Pointer ID
      // Ops[2] ... Ops[n] = Optional Memory Access
      //
      // TODO: Do we need to implement Optional Memory Access???
      Ops.clear();
      Ops << MkId(lookupType(ArgTy)) << MkId(PointerID);

      VMap[&Arg] = nextID;
      auto *Inst = new SPIRVInstruction(spv::OpLoad, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
    }
  }
}

void SPIRVProducerPass::GenerateEntryPointInitialStores() {
  // Work around a driver bug.  Initializers on Private variables might not
  // work. So the start of the kernel should store the initializer value to the
  // variables.  Yes, *every* entry point pays this cost if *any* entry point
  // uses this builtin.  At this point I judge this to be an acceptable tradeoff
  // of complexity vs. runtime, for a broken driver.
  // TODO(dneto): Remove this at some point once fixed drivers are widely available.
  if (WorkgroupSizeVarID) {
    assert(WorkgroupSizeValueID);

    SPIRVOperandList Ops;
    Ops << MkId(WorkgroupSizeVarID) << MkId(WorkgroupSizeValueID);

    auto *Inst = new SPIRVInstruction(spv::OpStore, Ops);
    getSPIRVInstList().push_back(Inst);
  }
}

void SPIRVProducerPass::GenerateFuncBody(Function &F) {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();

  const bool IsKernel = F.getCallingConv() == CallingConv::SPIR_KERNEL;

  for (BasicBlock &BB : F) {
    // Register BasicBlock to ValueMap.
    VMap[&BB] = nextID;

    //
    // Generate OpLabel for Basic Block.
    //
    SPIRVOperandList Ops;
    auto *Inst = new SPIRVInstruction(spv::OpLabel, nextID++, Ops);
    SPIRVInstList.push_back(Inst);

    // OpVariable instructions must come first.
    for (Instruction &I : BB) {
      if (isa<AllocaInst>(I)) {
        GenerateInstruction(I);
      }
    }

    if (&BB == &F.getEntryBlock() && IsKernel) {
      if (clspv::Option::HackInitializers()) {
        GenerateEntryPointInitialStores();
      }
      GenerateInstForArg(F);
    }

    for (Instruction &I : BB) {
      if (!isa<AllocaInst>(I)) {
        GenerateInstruction(I);
      }
    }
  }
}

spv::Op SPIRVProducerPass::GetSPIRVCmpOpcode(CmpInst *I) {
  const std::map<CmpInst::Predicate, spv::Op> Map = {
      {CmpInst::ICMP_EQ, spv::OpIEqual},
      {CmpInst::ICMP_NE, spv::OpINotEqual},
      {CmpInst::ICMP_UGT, spv::OpUGreaterThan},
      {CmpInst::ICMP_UGE, spv::OpUGreaterThanEqual},
      {CmpInst::ICMP_ULT, spv::OpULessThan},
      {CmpInst::ICMP_ULE, spv::OpULessThanEqual},
      {CmpInst::ICMP_SGT, spv::OpSGreaterThan},
      {CmpInst::ICMP_SGE, spv::OpSGreaterThanEqual},
      {CmpInst::ICMP_SLT, spv::OpSLessThan},
      {CmpInst::ICMP_SLE, spv::OpSLessThanEqual},
      {CmpInst::FCMP_OEQ, spv::OpFOrdEqual},
      {CmpInst::FCMP_OGT, spv::OpFOrdGreaterThan},
      {CmpInst::FCMP_OGE, spv::OpFOrdGreaterThanEqual},
      {CmpInst::FCMP_OLT, spv::OpFOrdLessThan},
      {CmpInst::FCMP_OLE, spv::OpFOrdLessThanEqual},
      {CmpInst::FCMP_ONE, spv::OpFOrdNotEqual},
      {CmpInst::FCMP_UEQ, spv::OpFUnordEqual},
      {CmpInst::FCMP_UGT, spv::OpFUnordGreaterThan},
      {CmpInst::FCMP_UGE, spv::OpFUnordGreaterThanEqual},
      {CmpInst::FCMP_ULT, spv::OpFUnordLessThan},
      {CmpInst::FCMP_ULE, spv::OpFUnordLessThanEqual},
      {CmpInst::FCMP_UNE, spv::OpFUnordNotEqual}};

  assert(0 != Map.count(I->getPredicate()));

  return Map.at(I->getPredicate());
}

spv::Op SPIRVProducerPass::GetSPIRVCastOpcode(Instruction &I) {
  const std::map<unsigned, spv::Op> Map{
      {Instruction::Trunc, spv::OpUConvert},
      {Instruction::ZExt, spv::OpUConvert},
      {Instruction::SExt, spv::OpSConvert},
      {Instruction::FPToUI, spv::OpConvertFToU},
      {Instruction::FPToSI, spv::OpConvertFToS},
      {Instruction::UIToFP, spv::OpConvertUToF},
      {Instruction::SIToFP, spv::OpConvertSToF},
      {Instruction::FPTrunc, spv::OpFConvert},
      {Instruction::FPExt, spv::OpFConvert},
      {Instruction::BitCast, spv::OpBitcast}};

  assert(0 != Map.count(I.getOpcode()));

  return Map.at(I.getOpcode());
}

spv::Op SPIRVProducerPass::GetSPIRVBinaryOpcode(Instruction &I) {
  if (I.getType()->isIntegerTy(1)) {
    switch (I.getOpcode()) {
    default:
      break;
    case Instruction::Or:
      return spv::OpLogicalOr;
    case Instruction::And:
      return spv::OpLogicalAnd;
    case Instruction::Xor:
      return spv::OpLogicalNotEqual;
    }
  }

  const std::map<unsigned, spv::Op> Map {
      {Instruction::Add, spv::OpIAdd},
      {Instruction::FAdd, spv::OpFAdd},
      {Instruction::Sub, spv::OpISub},
      {Instruction::FSub, spv::OpFSub},
      {Instruction::Mul, spv::OpIMul},
      {Instruction::FMul, spv::OpFMul},
      {Instruction::UDiv, spv::OpUDiv},
      {Instruction::SDiv, spv::OpSDiv},
      {Instruction::FDiv, spv::OpFDiv},
      {Instruction::URem, spv::OpUMod},
      {Instruction::SRem, spv::OpSRem},
      {Instruction::FRem, spv::OpFRem},
      {Instruction::Or, spv::OpBitwiseOr},
      {Instruction::Xor, spv::OpBitwiseXor},
      {Instruction::And, spv::OpBitwiseAnd},
      {Instruction::Shl, spv::OpShiftLeftLogical},
      {Instruction::LShr, spv::OpShiftRightLogical},
      {Instruction::AShr, spv::OpShiftRightArithmetic}};

  assert(0 != Map.count(I.getOpcode()));

  return Map.at(I.getOpcode());
}

void SPIRVProducerPass::GenerateInstruction(Instruction &I) {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();
  ValueToValueMapTy &ArgGVMap = getArgumentGVMap();
  ValueMapType &ArgGVIDMap = getArgumentGVIDMap();
  DeferredInstVecType &DeferredInsts = getDeferredInstVec();
  LLVMContext &Context = I.getParent()->getParent()->getParent()->getContext();

  // Register Instruction to ValueMap.
  if (0 == VMap[&I]) {
    VMap[&I] = nextID;
  }

  switch (I.getOpcode()) {
  default: {
    if (Instruction::isCast(I.getOpcode())) {
      //
      // Generate SPIRV instructions for cast operators.
      //


      auto Ty = I.getType();
      auto OpTy = I.getOperand(0)->getType();
      auto toI8 = Ty == Type::getInt8Ty(Context);
      auto fromI32 = OpTy == Type::getInt32Ty(Context);
      // Handle zext, sext and uitofp with i1 type specially.
      if ((I.getOpcode() == Instruction::ZExt ||
           I.getOpcode() == Instruction::SExt ||
           I.getOpcode() == Instruction::UIToFP) &&
          (OpTy->isIntegerTy(1) ||
           (OpTy->isVectorTy() &&
            OpTy->getVectorElementType()->isIntegerTy(1)))) {
        //
        // Generate OpSelect.
        //

        // Ops[0] = Result Type ID
        // Ops[1] = Condition ID
        // Ops[2] = True Constant ID
        // Ops[3] = False Constant ID
        SPIRVOperandList Ops;

        Ops << MkId(lookupType(I.getType()));

        uint32_t CondID = VMap[I.getOperand(0)];
        Ops << MkId(CondID);

        uint32_t TrueID = 0;
        if (I.getOpcode() == Instruction::ZExt) {
          APInt One(32, 1);
          TrueID = VMap[Constant::getIntegerValue(I.getType(), One)];
        } else if (I.getOpcode() == Instruction::SExt) {
          APInt MinusOne(32, UINT64_MAX, true);
          TrueID = VMap[Constant::getIntegerValue(I.getType(), MinusOne)];
        } else {
          TrueID = VMap[ConstantFP::get(Context, APFloat(1.0f))];
        }
        Ops << MkId(TrueID);

        uint32_t FalseID = 0;
        if (I.getOpcode() == Instruction::ZExt) {
          FalseID = VMap[Constant::getNullValue(I.getType())];
        } else if (I.getOpcode() == Instruction::SExt) {
          FalseID = VMap[Constant::getNullValue(I.getType())];
        } else {
          FalseID = VMap[ConstantFP::get(Context, APFloat(0.0f))];
        }
        Ops << MkId(FalseID);

        auto *Inst = new SPIRVInstruction(spv::OpSelect, nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      } else if (I.getOpcode() == Instruction::Trunc && fromI32 && toI8) {
        // The SPIR-V target type is a 32-bit int.  Keep only the bottom
        // 8 bits.
        // Before:
        //   %result = trunc i32 %a to i8
        // After
        //   %result = OpBitwiseAnd %uint %a %uint_255

        SPIRVOperandList Ops;

        Ops << MkId(lookupType(OpTy)) << MkId(VMap[I.getOperand(0)]);

        Type *UintTy = Type::getInt32Ty(Context);
        uint32_t MaskID = VMap[ConstantInt::get(UintTy, 255)];
        Ops << MkId(MaskID);

        auto *Inst = new SPIRVInstruction(spv::OpBitwiseAnd, nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      } else {
        // Ops[0] = Result Type ID
        // Ops[1] = Source Value ID
        SPIRVOperandList Ops;

        Ops << MkId(lookupType(I.getType())) << MkId(VMap[I.getOperand(0)]);

        auto *Inst = new SPIRVInstruction(GetSPIRVCastOpcode(I), nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      }
    } else if (isa<BinaryOperator>(I)) {
      //
      // Generate SPIRV instructions for binary operators.
      //

      // Handle xor with i1 type specially.
      if (I.getOpcode() == Instruction::Xor &&
          I.getType() == Type::getInt1Ty(Context) &&
          (isa<Constant>(I.getOperand(0)) || isa<Constant>(I.getOperand(1)))) {
        //
        // Generate OpLogicalNot.
        //
        // Ops[0] = Result Type ID
        // Ops[1] = Operand
        SPIRVOperandList Ops;

        Ops << MkId(lookupType(I.getType()));

        Value *CondV = I.getOperand(0);
        if (isa<Constant>(I.getOperand(0))) {
          CondV = I.getOperand(1);
        }
        Ops << MkId(VMap[CondV]);

        auto *Inst = new SPIRVInstruction(spv::OpLogicalNot, nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      } else {
        // Ops[0] = Result Type ID
        // Ops[1] = Operand 0
        // Ops[2] = Operand 1
        SPIRVOperandList Ops;

        Ops << MkId(lookupType(I.getType())) << MkId(VMap[I.getOperand(0)])
            << MkId(VMap[I.getOperand(1)]);

        auto *Inst =
            new SPIRVInstruction(GetSPIRVBinaryOpcode(I), nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      }
    } else {
      I.print(errs());
      llvm_unreachable("Unsupported instruction???");
    }
    break;
  }
  case Instruction::GetElementPtr: {
    auto &GlobalConstArgSet = getGlobalConstArgSet();

    //
    // Generate OpAccessChain.
    //
    GetElementPtrInst *GEP = cast<GetElementPtrInst>(&I);

    //
    // Generate OpAccessChain.
    //

    // Ops[0] = Result Type ID
    // Ops[1] = Base ID
    // Ops[2] ... Ops[n] = Indexes ID
    SPIRVOperandList Ops;

    PointerType* ResultType = cast<PointerType>(GEP->getType());
    if (GEP->getPointerAddressSpace() == AddressSpace::ModuleScopePrivate ||
        GlobalConstArgSet.count(GEP->getPointerOperand())) {
      // Use pointer type with private address space for global constant.
      Type *EleTy = I.getType()->getPointerElementType();
      ResultType = PointerType::get(EleTy, AddressSpace::ModuleScopePrivate);
    }

    Ops << MkId(lookupType(ResultType));

    // Check whether GEP's pointer operand is pointer argument.
    bool HasArgBasePointer = false;
    for (auto ArgGV : ArgGVMap) {
      if (ArgGV.first == GEP->getPointerOperand()) {
        if (isa<PointerType>(ArgGV.first->getType())) {
          HasArgBasePointer = true;
        } else {
          llvm_unreachable(
              "GEP's pointer operand is argument of non-poninter type???");
        }
      }
    }

    uint32_t BaseID;
    if (HasArgBasePointer) {
      // Point to global variable for argument directly.
      BaseID = ArgGVIDMap[GEP->getPointerOperand()];
    } else {
      BaseID = VMap[GEP->getPointerOperand()];
    }

    Ops << MkId(BaseID);

    if (HasArgBasePointer) {
      // If GEP's pointer operand is argument, add one more index for struct
      // type to wrap up argument type.
      Type *IdxTy = Type::getInt32Ty(Context);
      Ops << MkId(VMap[ConstantInt::get(IdxTy, 0)]);
    }

    //
    // Follows below rules for gep.
    //
    // 1. If gep's first index is 0 and gep's base is not kernel function's
    //    argument, generate OpAccessChain and ignore gep's first index.
    // 2. If gep's first index is not 0, generate OpPtrAccessChain and use gep's
    //    first index.
    // 3. If gep's first index is not constant, generate OpPtrAccessChain and
    //    use gep's first index.
    // 4. If it is not above case 1, 2 and 3, generate OpAccessChain and use
    //    gep's first index.
    //
    spv::Op Opcode = spv::OpAccessChain;
    unsigned offset = 0;
    if (ConstantInt *CstInt = dyn_cast<ConstantInt>(GEP->getOperand(1))) {
      if (CstInt->getZExtValue() == 0 && !HasArgBasePointer) {
        offset = 1;
      } else if (CstInt->getZExtValue() != 0 && !HasArgBasePointer) {
        Opcode = spv::OpPtrAccessChain;
      }
    } else if (!HasArgBasePointer) {
      Opcode = spv::OpPtrAccessChain;
    }

    if (Opcode == spv::OpPtrAccessChain) {
      setVariablePointers(true);
      // Do we need to generate ArrayStride?  Check against the GEP result type
      // rather than the pointer type of the base because when indexing into
      // an OpenCL program-scope constant, we'll swap out the LLVM base pointer
      // for something else in the SPIR-V.
      // E.g. see test/PointerAccessChain/pointer_index_is_constant_1.cl
      if (GetStorageClass(ResultType->getAddressSpace()) ==
          spv::StorageClassStorageBuffer) {
        // Save the need to generate an ArrayStride decoration.  But defer
        // generation until later, so we only make one decoration.
        getTypesNeedingArrayStride().insert(ResultType);
      }
    }

    for (auto II = GEP->idx_begin() + offset; II != GEP->idx_end(); II++) {
      Ops << MkId(VMap[*II]);
    }

    auto *Inst = new SPIRVInstruction(Opcode, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::ExtractValue: {
    ExtractValueInst *EVI = cast<ExtractValueInst>(&I);
    // Ops[0] = Result Type ID
    // Ops[1] = Composite ID
    // Ops[2] ... Ops[n] = Indexes (Literal Number)
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(I.getType()));

    uint32_t CompositeID = VMap[EVI->getAggregateOperand()];
    Ops << MkId(CompositeID);

    for (auto &Index : EVI->indices()) {
      Ops << MkNum(Index);
    }

    auto *Inst = new SPIRVInstruction(spv::OpCompositeExtract, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::InsertValue: {
    InsertValueInst *IVI = cast<InsertValueInst>(&I);
    // Ops[0] = Result Type ID
    // Ops[1] = Object ID
    // Ops[2] = Composite ID
    // Ops[3] ... Ops[n] = Indexes (Literal Number)
    SPIRVOperandList Ops;

    uint32_t ResTyID = lookupType(I.getType());
    Ops << MkId(ResTyID);

    uint32_t ObjectID = VMap[IVI->getInsertedValueOperand()];
    Ops << MkId(ObjectID);

    uint32_t CompositeID = VMap[IVI->getAggregateOperand()];
    Ops << MkId(CompositeID);

    for (auto &Index : IVI->indices()) {
      Ops << MkNum(Index);
    }

    auto *Inst = new SPIRVInstruction(spv::OpCompositeInsert, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::Select: {
    //
    // Generate OpSelect.
    //

    // Ops[0] = Result Type ID
    // Ops[1] = Condition ID
    // Ops[2] = True Constant ID
    // Ops[3] = False Constant ID
    SPIRVOperandList Ops;

    // Find SPIRV instruction for parameter type.
    auto Ty = I.getType();
    if (Ty->isPointerTy()) {
      auto PointeeTy = Ty->getPointerElementType();
      if (PointeeTy->isStructTy() &&
          dyn_cast<StructType>(PointeeTy)->isOpaque()) {
        Ty = PointeeTy;
      }
    }

    Ops << MkId(lookupType(Ty)) << MkId(VMap[I.getOperand(0)])
        << MkId(VMap[I.getOperand(1)]) << MkId(VMap[I.getOperand(2)]);

    auto *Inst = new SPIRVInstruction(spv::OpSelect, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::ExtractElement: {
    // Handle <4 x i8> type manually.
    Type *CompositeTy = I.getOperand(0)->getType();
    if (is4xi8vec(CompositeTy)) {
      //
      // Generate OpShiftRightLogical and OpBitwiseAnd for extractelement with
      // <4 x i8>.
      //

      //
      // Generate OpShiftRightLogical
      //
      // Ops[0] = Result Type ID
      // Ops[1] = Operand 0
      // Ops[2] = Operand 1
      //
      SPIRVOperandList Ops;

      Ops << MkId(lookupType(CompositeTy));

      uint32_t Op0ID = VMap[I.getOperand(0)];
      Ops << MkId(Op0ID);

      uint32_t Op1ID = 0;
      if (ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(1))) {
        // Handle constant index.
        uint64_t Idx = CI->getZExtValue();
        Value *ShiftAmount =
            ConstantInt::get(Type::getInt32Ty(Context), Idx * 8);
        Op1ID = VMap[ShiftAmount];
      } else {
        // Handle variable index.
        SPIRVOperandList TmpOps;

        TmpOps << MkId(lookupType(Type::getInt32Ty(Context)))
               << MkId(VMap[I.getOperand(1)]);

        ConstantInt *Cst8 = ConstantInt::get(Type::getInt32Ty(Context), 8);
        TmpOps << MkId(VMap[Cst8]);

        Op1ID = nextID;

        auto *TmpInst = new SPIRVInstruction(spv::OpIMul, nextID++, TmpOps);
        SPIRVInstList.push_back(TmpInst);
      }
      Ops << MkId(Op1ID);

      uint32_t ShiftID = nextID;

      auto *Inst =
          new SPIRVInstruction(spv::OpShiftRightLogical, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      //
      // Generate OpBitwiseAnd
      //
      // Ops[0] = Result Type ID
      // Ops[1] = Operand 0
      // Ops[2] = Operand 1
      //
      Ops.clear();

      Ops << MkId(lookupType(CompositeTy)) << MkId(ShiftID);

      Constant *CstFF = ConstantInt::get(Type::getInt32Ty(Context), 0xFF);
      Ops << MkId(VMap[CstFF]);

      // Reset mapping for this value to the result of the bitwise and.
      VMap[&I] = nextID;

      Inst = new SPIRVInstruction(spv::OpBitwiseAnd, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // Ops[0] = Result Type ID
    // Ops[1] = Composite ID
    // Ops[2] ... Ops[n] = Indexes (Literal Number)
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(I.getType())) << MkId(VMap[I.getOperand(0)]);

    spv::Op Opcode = spv::OpCompositeExtract;
    if (const ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(1))) {
      Ops << MkNum(static_cast<uint32_t>(CI->getZExtValue()));
    } else {
      Ops << MkId(VMap[I.getOperand(1)]);
      Opcode = spv::OpVectorExtractDynamic;
    }

    auto *Inst = new SPIRVInstruction(Opcode, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::InsertElement: {
    // Handle <4 x i8> type manually.
    Type *CompositeTy = I.getOperand(0)->getType();
    if (is4xi8vec(CompositeTy)) {
      Constant *CstFF = ConstantInt::get(Type::getInt32Ty(Context), 0xFF);
      uint32_t CstFFID = VMap[CstFF];

      uint32_t ShiftAmountID = 0;
      if (ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(2))) {
        // Handle constant index.
        uint64_t Idx = CI->getZExtValue();
        Value *ShiftAmount =
            ConstantInt::get(Type::getInt32Ty(Context), Idx * 8);
        ShiftAmountID = VMap[ShiftAmount];
      } else {
        // Handle variable index.
        SPIRVOperandList TmpOps;

        TmpOps << MkId(lookupType(Type::getInt32Ty(Context)))
               << MkId(VMap[I.getOperand(2)]);

        ConstantInt *Cst8 = ConstantInt::get(Type::getInt32Ty(Context), 8);
        TmpOps << MkId(VMap[Cst8]);

        ShiftAmountID = nextID;

        auto *TmpInst = new SPIRVInstruction(spv::OpIMul, nextID++, TmpOps);
        SPIRVInstList.push_back(TmpInst);
      }

      //
      // Generate mask operations.
      //

      // ShiftLeft mask according to index of insertelement.
      SPIRVOperandList Ops;

      const uint32_t ResTyID = lookupType(CompositeTy);
      Ops << MkId(ResTyID) << MkId(CstFFID) << MkId(ShiftAmountID);

      uint32_t MaskID = nextID;

      auto *Inst = new SPIRVInstruction(spv::OpShiftLeftLogical, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      // Inverse mask.
      Ops.clear();
      Ops << MkId(ResTyID) << MkId(MaskID);

      uint32_t InvMaskID = nextID;

      Inst = new SPIRVInstruction(spv::OpNot, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      // Apply mask.
      Ops.clear();
      Ops << MkId(ResTyID) << MkId(VMap[I.getOperand(0)]) << MkId(InvMaskID);

      uint32_t OrgValID = nextID;

      Inst = new SPIRVInstruction(spv::OpBitwiseAnd, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      // Create correct value according to index of insertelement.
      Ops.clear();
      Ops << MkId(ResTyID) << MkId(VMap[I.getOperand(1)]) << MkId(ShiftAmountID);

      uint32_t InsertValID = nextID;

      Inst = new SPIRVInstruction(spv::OpShiftLeftLogical, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      // Insert value to original value.
      Ops.clear();
      Ops << MkId(ResTyID) << MkId(OrgValID) << MkId(InsertValID);

      VMap[&I] = nextID;

      Inst = new SPIRVInstruction(spv::OpBitwiseOr, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      break;
    }

    // Ops[0] = Result Type ID
    // Ops[1] = Object ID
    // Ops[2] = Composite ID
    // Ops[3] ... Ops[n] = Indexes (Literal Number)
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(I.getType())) << MkId(VMap[I.getOperand(1)])
        << MkId(VMap[I.getOperand(0)]);

    spv::Op Opcode = spv::OpCompositeInsert;
    if (const ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(2))) {
      const auto value = CI->getZExtValue();
      assert(value <= UINT32_MAX);
      Ops << MkNum(static_cast<uint32_t>(value));
    } else {
      Ops << MkId(VMap[I.getOperand(1)]);
      Opcode = spv::OpVectorInsertDynamic;
    }

    auto *Inst = new SPIRVInstruction(Opcode, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::ShuffleVector: {
    // Ops[0] = Result Type ID
    // Ops[1] = Vector 1 ID
    // Ops[2] = Vector 2 ID
    // Ops[3] ... Ops[n] = Components (Literal Number)
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(I.getType())) << MkId(VMap[I.getOperand(0)])
        << MkId(VMap[I.getOperand(1)]);

    uint64_t NumElements = 0;
    if (Constant *Cst = dyn_cast<Constant>(I.getOperand(2))) {
      NumElements = cast<VectorType>(Cst->getType())->getNumElements();

      if (Cst->isNullValue()) {
        for (unsigned i = 0; i < NumElements; i++) {
          Ops << MkNum(0);
        }
      } else if (const ConstantDataSequential *CDS =
                     dyn_cast<ConstantDataSequential>(Cst)) {
        for (unsigned i = 0; i < CDS->getNumElements(); i++) {
          std::vector<uint32_t> LiteralNum;
          const auto value = CDS->getElementAsInteger(i);
          assert(value <= UINT32_MAX);
          Ops << MkNum(static_cast<uint32_t>(value));
        }
      } else if (const ConstantVector *CV = dyn_cast<ConstantVector>(Cst)) {
        for (unsigned i = 0; i < CV->getNumOperands(); i++) {
          auto Op = CV->getOperand(i);

          uint32_t literal = 0;

          if (auto CI = dyn_cast<ConstantInt>(Op)) {
            literal = static_cast<uint32_t>(CI->getZExtValue());
          } else if (auto UI = dyn_cast<UndefValue>(Op)) {
            literal = 0xFFFFFFFFu;
          } else {
            Op->print(errs());
            llvm_unreachable("Unsupported element in ConstantVector!");
          }

          Ops << MkNum(literal);
        }
      } else {
        Cst->print(errs());
        llvm_unreachable("Unsupported constant mask in ShuffleVector!");
      }
    }

    auto *Inst = new SPIRVInstruction(spv::OpVectorShuffle, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::ICmp:
  case Instruction::FCmp: {
    CmpInst *CmpI = cast<CmpInst>(&I);

    // Pointer equality is invalid.
    Type* ArgTy = CmpI->getOperand(0)->getType();
    if (isa<PointerType>(ArgTy)) {
      CmpI->print(errs());
      std::string name = I.getParent()->getParent()->getName();
      errs()
          << "\nPointer equality test is not supported by SPIR-V for Vulkan, "
          << "in function " << name << "\n";
      llvm_unreachable("Pointer equality check is invalid");
      break;
    }

    // Ops[0] = Result Type ID
    // Ops[1] = Operand 1 ID
    // Ops[2] = Operand 2 ID
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(CmpI->getType())) << MkId(VMap[CmpI->getOperand(0)])
        << MkId(VMap[CmpI->getOperand(1)]);

    spv::Op Opcode = GetSPIRVCmpOpcode(CmpI);
    auto *Inst = new SPIRVInstruction(Opcode, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::Br: {
    // Branch instrucion is deferred because it needs label's ID. Record slot's
    // location on SPIRVInstructionList.
    DeferredInsts.push_back(
        std::make_tuple(&I, --SPIRVInstList.end(), 0 /* No id */));
    break;
  }
  case Instruction::Switch: {
    I.print(errs());
    llvm_unreachable("Unsupported instruction???");
    break;
  }
  case Instruction::IndirectBr: {
    I.print(errs());
    llvm_unreachable("Unsupported instruction???");
    break;
  }
  case Instruction::PHI: {
    // Branch instrucion is deferred because it needs label's ID. Record slot's
    // location on SPIRVInstructionList.
    DeferredInsts.push_back(
        std::make_tuple(&I, --SPIRVInstList.end(), nextID++));
    break;
  }
  case Instruction::Alloca: {
    //
    // Generate OpVariable.
    //
    // Ops[0] : Result Type ID
    // Ops[1] : Storage Class
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(I.getType())) << MkNum(spv::StorageClassFunction);

    auto *Inst = new SPIRVInstruction(spv::OpVariable, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::Load: {
    LoadInst *LD = cast<LoadInst>(&I);
    //
    // Generate OpLoad.
    //

    uint32_t ResTyID = lookupType(LD->getType());
    uint32_t PointerID = VMap[LD->getPointerOperand()];

    // This is a hack to work around what looks like a driver bug.
    // When we're loading from the special variable holding the WorkgroupSize
    // builtin value, use an OpBitWiseAnd of the value's ID rather than
    // generating a load.
    // TODO(dneto): Remove this awful hack once drivers are fixed.
    if (PointerID == WorkgroupSizeVarID) {
      // Generate a bitwise-and of the original value with itself.
      // We should have been able to get away with just an OpCopyObject,
      // but we need something more complex to get past certain driver bugs.
      // This is ridiculous, but necessary.
      // TODO(dneto): Revisit this once drivers fix their bugs.

      SPIRVOperandList Ops;
      Ops << MkId(ResTyID) << MkId(WorkgroupSizeValueID)
          << MkId(WorkgroupSizeValueID);

      auto *Inst = new SPIRVInstruction(spv::OpBitwiseAnd, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // This is the normal path.  Generate a load.

    // Ops[0] = Result Type ID
    // Ops[1] = Pointer ID
    // Ops[2] ... Ops[n] = Optional Memory Access
    //
    // TODO: Do we need to implement Optional Memory Access???

    SPIRVOperandList Ops;
    Ops << MkId(ResTyID) << MkId(PointerID);

    auto *Inst = new SPIRVInstruction(spv::OpLoad, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::Store: {
    StoreInst *ST = cast<StoreInst>(&I);
    //
    // Generate OpStore.
    //

    // Ops[0] = Pointer ID
    // Ops[1] = Object ID
    // Ops[2] ... Ops[n] = Optional Memory Access (later???)
    //
    // TODO: Do we need to implement Optional Memory Access???
    SPIRVOperandList Ops;
    Ops << MkId(VMap[ST->getPointerOperand()])
        << MkId(VMap[ST->getValueOperand()]);

    auto *Inst = new SPIRVInstruction(spv::OpStore, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::AtomicCmpXchg: {
    I.print(errs());
    llvm_unreachable("Unsupported instruction???");
    break;
  }
  case Instruction::AtomicRMW: {
    AtomicRMWInst *AtomicRMW = dyn_cast<AtomicRMWInst>(&I);

    spv::Op opcode;

    switch (AtomicRMW->getOperation()) {
    default:
      I.print(errs());
      llvm_unreachable("Unsupported instruction???");
    case llvm::AtomicRMWInst::Add:
      opcode = spv::OpAtomicIAdd;
      break;
    case llvm::AtomicRMWInst::Sub:
      opcode = spv::OpAtomicISub;
      break;
    case llvm::AtomicRMWInst::Xchg:
      opcode = spv::OpAtomicExchange;
      break;
    case llvm::AtomicRMWInst::Min:
      opcode = spv::OpAtomicSMin;
      break;
    case llvm::AtomicRMWInst::Max:
      opcode = spv::OpAtomicSMax;
      break;
    case llvm::AtomicRMWInst::UMin:
      opcode = spv::OpAtomicUMin;
      break;
    case llvm::AtomicRMWInst::UMax:
      opcode = spv::OpAtomicUMax;
      break;
    case llvm::AtomicRMWInst::And:
      opcode = spv::OpAtomicAnd;
      break;
    case llvm::AtomicRMWInst::Or:
      opcode = spv::OpAtomicOr;
      break;
    case llvm::AtomicRMWInst::Xor:
      opcode = spv::OpAtomicXor;
      break;
    }

    //
    // Generate OpAtomic*.
    //
    SPIRVOperandList Ops;

    Ops << MkId(lookupType(I.getType()))
        << MkId(VMap[AtomicRMW->getPointerOperand()]);

    auto IntTy = Type::getInt32Ty(I.getContext());
    const auto ConstantScopeDevice = ConstantInt::get(IntTy, spv::ScopeDevice);
    Ops << MkId(VMap[ConstantScopeDevice]);

    const auto ConstantMemorySemantics = ConstantInt::get(
        IntTy, spv::MemorySemanticsUniformMemoryMask |
                   spv::MemorySemanticsSequentiallyConsistentMask);
    Ops << MkId(VMap[ConstantMemorySemantics]);

    Ops << MkId(VMap[AtomicRMW->getValOperand()]);

    VMap[&I] = nextID;

    auto *Inst = new SPIRVInstruction(opcode, nextID++, Ops);
    SPIRVInstList.push_back(Inst);
    break;
  }
  case Instruction::Fence: {
    I.print(errs());
    llvm_unreachable("Unsupported instruction???");
    break;
  }
  case Instruction::Call: {
    CallInst *Call = dyn_cast<CallInst>(&I);
    Function *Callee = Call->getCalledFunction();

    // Sampler initializers become a load of the corresponding sampler.
    if (Callee->getName().equals("__translate_sampler_initializer")) {
      // Check that the sampler map was definitely used though.
      if (0 == getSamplerMap().size()) {
        errs() << "error: kernel uses a literal sampler but option -samplermap "
                  "has not been specified\n";
        llvm_unreachable("Sampler literal in source without sampler map!");
      }

      SPIRVOperandList Ops;

      Ops << MkId(lookupType(SamplerTy->getPointerElementType()))
          << MkId(VMap[Call]);

      VMap[Call] = nextID;
      auto *Inst = new SPIRVInstruction(spv::OpLoad, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      break;
    }

    if (Callee->getName().startswith("spirv.atomic")) {
      spv::Op opcode = StringSwitch<spv::Op>(Callee->getName())
                           .Case("spirv.atomic_add", spv::OpAtomicIAdd)
                           .Case("spirv.atomic_sub", spv::OpAtomicISub)
                           .Case("spirv.atomic_exchange", spv::OpAtomicExchange)
                           .Case("spirv.atomic_inc", spv::OpAtomicIIncrement)
                           .Case("spirv.atomic_dec", spv::OpAtomicIDecrement)
                           .Case("spirv.atomic_compare_exchange",
                                 spv::OpAtomicCompareExchange)
                           .Case("spirv.atomic_umin", spv::OpAtomicUMin)
                           .Case("spirv.atomic_smin", spv::OpAtomicSMin)
                           .Case("spirv.atomic_umax", spv::OpAtomicUMax)
                           .Case("spirv.atomic_smax", spv::OpAtomicSMax)
                           .Case("spirv.atomic_and", spv::OpAtomicAnd)
                           .Case("spirv.atomic_or", spv::OpAtomicOr)
                           .Case("spirv.atomic_xor", spv::OpAtomicXor)
                           .Default(spv::OpNop);

      //
      // Generate OpAtomic*.
      //
      SPIRVOperandList Ops;

      Ops << MkId(lookupType(I.getType()));

      for (unsigned i = 0; i < Call->getNumArgOperands(); i++) {
        Ops << MkId(VMap[Call->getArgOperand(i)]);
      }

      VMap[&I] = nextID;

      auto *Inst = new SPIRVInstruction(opcode, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    if (Callee->getName().startswith("_Z3dot")) {
      // If the argument is a vector type, generate OpDot
      if (Call->getArgOperand(0)->getType()->isVectorTy()) {
        //
        // Generate OpDot.
        //
        SPIRVOperandList Ops;

        Ops << MkId(lookupType(I.getType()));

        for (unsigned i = 0; i < Call->getNumArgOperands(); i++) {
          Ops << MkId(VMap[Call->getArgOperand(i)]);
        }

        VMap[&I] = nextID;

        auto *Inst = new SPIRVInstruction(spv::OpDot, nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      } else {
        //
        // Generate OpFMul.
        //
        SPIRVOperandList Ops;

        Ops << MkId(lookupType(I.getType()));

        for (unsigned i = 0; i < Call->getNumArgOperands(); i++) {
          Ops << MkId(VMap[Call->getArgOperand(i)]);
        }

        VMap[&I] = nextID;

        auto *Inst = new SPIRVInstruction(spv::OpFMul, nextID++, Ops);
        SPIRVInstList.push_back(Inst);
      }
      break;
    }

    if (Callee->getName().startswith("_Z4fmod")) {
      // OpenCL fmod(x,y) is x - y * trunc(x/y)
      // The sign for a non-zero result is taken from x.
      // (Try an example.)
      // So translate to OpFRem

      SPIRVOperandList Ops;

      Ops << MkId(lookupType(I.getType()));

      for (unsigned i = 0; i < Call->getNumArgOperands(); i++) {
        Ops << MkId(VMap[Call->getArgOperand(i)]);
      }

      VMap[&I] = nextID;

      auto *Inst = new SPIRVInstruction(spv::OpFRem, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // spirv.store_null.* intrinsics become OpStore's.
    if (Callee->getName().startswith("spirv.store_null")) {
      //
      // Generate OpStore.
      //

      // Ops[0] = Pointer ID
      // Ops[1] = Object ID
      // Ops[2] ... Ops[n]
      SPIRVOperandList Ops;

      uint32_t PointerID = VMap[Call->getArgOperand(0)];
      uint32_t ObjectID = VMap[Call->getArgOperand(1)];
      Ops << MkId(PointerID) << MkId(ObjectID);

      SPIRVInstList.push_back(new SPIRVInstruction(spv::OpStore, Ops));

      break;
    }

    // spirv.copy_memory.* intrinsics become OpMemoryMemory's.
    if (Callee->getName().startswith("spirv.copy_memory")) {
      //
      // Generate OpCopyMemory.
      //

      // Ops[0] = Dst ID
      // Ops[1] = Src ID
      // Ops[2] = Memory Access
      // Ops[3] = Alignment

      auto IsVolatile =
          dyn_cast<ConstantInt>(Call->getArgOperand(3))->getZExtValue() != 0;

      auto VolatileMemoryAccess = (IsVolatile) ? spv::MemoryAccessVolatileMask
                                               : spv::MemoryAccessMaskNone;

      auto MemoryAccess = VolatileMemoryAccess | spv::MemoryAccessAlignedMask;

      auto Alignment =
          dyn_cast<ConstantInt>(Call->getArgOperand(2))->getZExtValue();

      SPIRVOperandList Ops;
      Ops << MkId(VMap[Call->getArgOperand(0)])
          << MkId(VMap[Call->getArgOperand(1)]) << MkNum(MemoryAccess)
          << MkNum(static_cast<uint32_t>(Alignment));

      auto *Inst = new SPIRVInstruction(spv::OpCopyMemory, Ops);

      SPIRVInstList.push_back(Inst);

      break;
    }

    // Nothing to do for abs with uint. Map abs's operand ID to VMap for abs
    // with unit.
    if (Callee->getName().equals("_Z3absj") ||
        Callee->getName().equals("_Z3absDv2_j") ||
        Callee->getName().equals("_Z3absDv3_j") ||
        Callee->getName().equals("_Z3absDv4_j")) {
      VMap[&I] = VMap[Call->getOperand(0)];
      break;
    }

    // barrier is converted to OpControlBarrier
    if (Callee->getName().equals("__spirv_control_barrier")) {
      //
      // Generate OpControlBarrier.
      //
      // Ops[0] = Execution Scope ID
      // Ops[1] = Memory Scope ID
      // Ops[2] = Memory Semantics ID
      //
      Value *ExecutionScope = Call->getArgOperand(0);
      Value *MemoryScope = Call->getArgOperand(1);
      Value *MemorySemantics = Call->getArgOperand(2);

      SPIRVOperandList Ops;
      Ops << MkId(VMap[ExecutionScope]) << MkId(VMap[MemoryScope])
          << MkId(VMap[MemorySemantics]);

      SPIRVInstList.push_back(new SPIRVInstruction(spv::OpControlBarrier, Ops));
      break;
    }

    // memory barrier is converted to OpMemoryBarrier
    if (Callee->getName().equals("__spirv_memory_barrier")) {
      //
      // Generate OpMemoryBarrier.
      //
      // Ops[0] = Memory Scope ID
      // Ops[1] = Memory Semantics ID
      //
      SPIRVOperandList Ops;

      uint32_t MemoryScopeID = VMap[Call->getArgOperand(0)];
      uint32_t MemorySemanticsID = VMap[Call->getArgOperand(1)];

      Ops << MkId(MemoryScopeID) << MkId(MemorySemanticsID);

      auto *Inst = new SPIRVInstruction(spv::OpMemoryBarrier, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // isinf is converted to OpIsInf
    if (Callee->getName().equals("__spirv_isinff") ||
        Callee->getName().equals("__spirv_isinfDv2_f") ||
        Callee->getName().equals("__spirv_isinfDv3_f") ||
        Callee->getName().equals("__spirv_isinfDv4_f")) {
      //
      // Generate OpIsInf.
      //
      // Ops[0] = Result Type ID
      // Ops[1] = X ID
      //
      SPIRVOperandList Ops;

      Ops << MkId(lookupType(I.getType()))
          << MkId(VMap[Call->getArgOperand(0)]);

      VMap[&I] = nextID;

      auto *Inst = new SPIRVInstruction(spv::OpIsInf, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // isnan is converted to OpIsNan
    if (Callee->getName().equals("__spirv_isnanf") ||
        Callee->getName().equals("__spirv_isnanDv2_f") ||
        Callee->getName().equals("__spirv_isnanDv3_f") ||
        Callee->getName().equals("__spirv_isnanDv4_f")) {
      //
      // Generate OpIsInf.
      //
      // Ops[0] = Result Type ID
      // Ops[1] = X ID
      //
      SPIRVOperandList Ops;

      Ops << MkId(lookupType(I.getType()))
          << MkId(VMap[Call->getArgOperand(0)]);

      VMap[&I] = nextID;

      auto *Inst = new SPIRVInstruction(spv::OpIsNan, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // all is converted to OpAll
    if (Callee->getName().equals("__spirv_allDv2_i") ||
        Callee->getName().equals("__spirv_allDv3_i") ||
        Callee->getName().equals("__spirv_allDv4_i")) {
      //
      // Generate OpAll.
      //
      // Ops[0] = Result Type ID
      // Ops[1] = Vector ID
      //
      SPIRVOperandList Ops;

      Ops << MkId(lookupType(I.getType()))
          << MkId(VMap[Call->getArgOperand(0)]);

      VMap[&I] = nextID;

      auto *Inst = new SPIRVInstruction(spv::OpAll, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // any is converted to OpAny
    if (Callee->getName().equals("__spirv_anyDv2_i") ||
        Callee->getName().equals("__spirv_anyDv3_i") ||
        Callee->getName().equals("__spirv_anyDv4_i")) {
      //
      // Generate OpAny.
      //
      // Ops[0] = Result Type ID
      // Ops[1] = Vector ID
      //
      SPIRVOperandList Ops;

      Ops << MkId(lookupType(I.getType()))
          << MkId(VMap[Call->getArgOperand(0)]);

      VMap[&I] = nextID;

      auto *Inst = new SPIRVInstruction(spv::OpAny, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // read_image is converted to OpSampledImage and OpImageSampleExplicitLod.
    // Additionally, OpTypeSampledImage is generated.
    if (Callee->getName().equals(
            "_Z11read_imagef14ocl_image2d_ro11ocl_samplerDv2_f") ||
        Callee->getName().equals(
            "_Z11read_imagef14ocl_image3d_ro11ocl_samplerDv4_f")) {
      //
      // Generate OpSampledImage.
      //
      // Ops[0] = Result Type ID
      // Ops[1] = Image ID
      // Ops[2] = Sampler ID
      //
      SPIRVOperandList Ops;

      Value *Image = Call->getArgOperand(0);
      Value *Sampler = Call->getArgOperand(1);
      Value *Coordinate = Call->getArgOperand(2);

      TypeMapType &OpImageTypeMap = getImageTypeMap();
      Type *ImageTy = Image->getType()->getPointerElementType();
      uint32_t ImageTyID = OpImageTypeMap[ImageTy];
      uint32_t ImageID = VMap[Image];
      uint32_t SamplerID = VMap[Sampler];

      Ops << MkId(ImageTyID) << MkId(ImageID) << MkId(SamplerID);

      uint32_t SampledImageID = nextID;

      auto *Inst = new SPIRVInstruction(spv::OpSampledImage, nextID++, Ops);
      SPIRVInstList.push_back(Inst);

      //
      // Generate OpImageSampleExplicitLod.
      //
      // Ops[0] = Result Type ID
      // Ops[1] = Sampled Image ID
      // Ops[2] = Coordinate ID
      // Ops[3] = Image Operands Type ID
      // Ops[4] ... Ops[n] = Operands ID
      //
      Ops.clear();

      Ops << MkId(lookupType(Call->getType())) << MkId(SampledImageID)
          << MkId(VMap[Coordinate]) << MkNum(spv::ImageOperandsLodMask);

      Constant *CstFP0 = ConstantFP::get(Context, APFloat(0.0f));
      Ops << MkId(VMap[CstFP0]);

      VMap[&I] = nextID;

      Inst = new SPIRVInstruction(spv::OpImageSampleExplicitLod, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // write_imagef is mapped to OpImageWrite.
    if (Callee->getName().equals(
            "_Z12write_imagef14ocl_image2d_woDv2_iDv4_f") ||
        Callee->getName().equals(
            "_Z12write_imagef14ocl_image3d_woDv4_iDv4_f")) {
      //
      // Generate OpImageWrite.
      //
      // Ops[0] = Image ID
      // Ops[1] = Coordinate ID
      // Ops[2] = Texel ID
      // Ops[3] = (Optional) Image Operands Type (Literal Number)
      // Ops[4] ... Ops[n] = (Optional) Operands ID
      //
      SPIRVOperandList Ops;

      Value *Image = Call->getArgOperand(0);
      Value *Coordinate = Call->getArgOperand(1);
      Value *Texel = Call->getArgOperand(2);

      uint32_t ImageID = VMap[Image];
      uint32_t CoordinateID = VMap[Coordinate];
      uint32_t TexelID = VMap[Texel];
      Ops << MkId(ImageID) << MkId(CoordinateID) << MkId(TexelID);

      auto *Inst = new SPIRVInstruction(spv::OpImageWrite, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // get_image_width is mapped to OpImageQuerySize
    if (Callee->getName().equals("_Z15get_image_width14ocl_image2d_ro") ||
        Callee->getName().equals("_Z15get_image_width14ocl_image2d_wo") ||
        Callee->getName().equals("_Z16get_image_height14ocl_image2d_ro") ||
        Callee->getName().equals("_Z16get_image_height14ocl_image2d_wo")) {
      //
      // Generate OpImageQuerySize, then pull out the right component.
      // Assume 2D image for now.
      //
      // Ops[0] = Image ID
      //
      // %sizes = OpImageQuerySizes %uint2 %im
      // %result = OpCompositeExtract %uint %sizes 0-or-1
      SPIRVOperandList Ops;

      // Implement:
      //     %sizes = OpImageQuerySizes %uint2 %im
      uint32_t SizesTypeID =
          TypeMap[VectorType::get(Type::getInt32Ty(Context), 2)];
      Value *Image = Call->getArgOperand(0);
      uint32_t ImageID = VMap[Image];
      Ops << MkId(SizesTypeID) << MkId(ImageID);

      uint32_t SizesID = nextID++;
      auto *QueryInst =
          new SPIRVInstruction(spv::OpImageQuerySize, SizesID, Ops);
      SPIRVInstList.push_back(QueryInst);

      // Reset value map entry since we generated an intermediate instruction.
      VMap[&I] = nextID;

      // Implement:
      //     %result = OpCompositeExtract %uint %sizes 0-or-1
      Ops.clear();
      Ops << MkId(TypeMap[I.getType()]) << MkId(SizesID);

      uint32_t component = Callee->getName().contains("height") ? 1 : 0;
      Ops << MkNum(component);

      auto *Inst = new SPIRVInstruction(spv::OpCompositeExtract, nextID++, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }

    // Call instrucion is deferred because it needs function's ID. Record
    // slot's location on SPIRVInstructionList.
    DeferredInsts.push_back(
        std::make_tuple(&I, --SPIRVInstList.end(), nextID++));

    // Check whether the implementation of this call uses an extended
    // instruction plus one more value-producing instruction.  If so, then
    // reserve the id for the extra value-producing slot.
    glsl::ExtInst EInst = getIndirectExtInstEnum(Callee->getName());
    if (EInst != kGlslExtInstBad) {
      // Reserve a spot for the extra value.
      // Increase nextID.
      VMap[&I] = nextID;
      nextID++;
    }
    break;
  }
  case Instruction::Ret: {
    unsigned NumOps = I.getNumOperands();
    if (NumOps == 0) {
      //
      // Generate OpReturn.
      //
      SPIRVInstList.push_back(new SPIRVInstruction(spv::OpReturn, {}));
    } else {
      //
      // Generate OpReturnValue.
      //

      // Ops[0] = Return Value ID
      SPIRVOperandList Ops;

      Ops << MkId(VMap[I.getOperand(0)]);

      auto *Inst = new SPIRVInstruction(spv::OpReturnValue, Ops);
      SPIRVInstList.push_back(Inst);
      break;
    }
    break;
  }
  }
}

void SPIRVProducerPass::GenerateFuncEpilogue() {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();

  //
  // Generate OpFunctionEnd
  //

  auto *Inst = new SPIRVInstruction(spv::OpFunctionEnd, {});
  SPIRVInstList.push_back(Inst);
}

bool SPIRVProducerPass::is4xi8vec(Type *Ty) const {
  LLVMContext &Context = Ty->getContext();
  if (Ty->isVectorTy()) {
    if (Ty->getVectorElementType() == Type::getInt8Ty(Context) &&
        Ty->getVectorNumElements() == 4) {
      return true;
    }
  }

  return false;
}

uint32_t SPIRVProducerPass::GetI32Zero() {
  if (0 == constant_i32_zero_id_) {
    llvm_unreachable("Requesting a 32-bit integer constant but it is not "
                     "defined in the SPIR-V module");
  }
  return constant_i32_zero_id_;
}

void SPIRVProducerPass::HandleDeferredInstruction() {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();
  ValueMapType &VMap = getValueMap();
  DeferredInstVecType &DeferredInsts = getDeferredInstVec();

  for (auto DeferredInst = DeferredInsts.rbegin();
       DeferredInst != DeferredInsts.rend(); ++DeferredInst) {
    Value *Inst = std::get<0>(*DeferredInst);
    SPIRVInstructionList::iterator InsertPoint = ++std::get<1>(*DeferredInst);
    if (InsertPoint != SPIRVInstList.end()) {
      while ((*InsertPoint)->getOpcode() == spv::OpPhi) {
        ++InsertPoint;
      }
    }

    if (BranchInst *Br = dyn_cast<BranchInst>(Inst)) {
      // Check whether basic block, which has this branch instruction, is loop
      // header or not. If it is loop header, generate OpLoopMerge and
      // OpBranchConditional.
      Function *Func = Br->getParent()->getParent();
      DominatorTree &DT =
          getAnalysis<DominatorTreeWrapperPass>(*Func).getDomTree();
      const LoopInfo &LI =
          getAnalysis<LoopInfoWrapperPass>(*Func).getLoopInfo();

      BasicBlock *BrBB = Br->getParent();
      if (LI.isLoopHeader(BrBB)) {
        Value *ContinueBB = nullptr;
        Value *MergeBB = nullptr;

        Loop *L = LI.getLoopFor(BrBB);
        MergeBB = L->getExitBlock();
        if (!MergeBB) {
          // StructurizeCFG pass converts CFG into triangle shape and the cfg
          // has regions with single entry/exit. As a result, loop should not
          // have multiple exits.
          llvm_unreachable("Loop has multiple exits???");
        }

        if (L->isLoopLatch(BrBB)) {
          ContinueBB = BrBB;
        } else {
          // From SPIR-V spec 2.11, Continue Target must dominate that back-edge
          // block.
          BasicBlock *Header = L->getHeader();
          BasicBlock *Latch = L->getLoopLatch();
          for (BasicBlock *BB : L->blocks()) {
            if (BB == Header) {
              continue;
            }

            // Check whether block dominates block with back-edge.
            if (DT.dominates(BB, Latch)) {
              ContinueBB = BB;
            }
          }

          if (!ContinueBB) {
            llvm_unreachable("Wrong continue block from loop");
          }
        }

        //
        // Generate OpLoopMerge.
        //
        // Ops[0] = Merge Block ID
        // Ops[1] = Continue Target ID
        // Ops[2] = Selection Control
        SPIRVOperandList Ops;

        // StructurizeCFG pass already manipulated CFG. Just use false block of
        // branch instruction as merge block.
        uint32_t MergeBBID = VMap[MergeBB];
        uint32_t ContinueBBID = VMap[ContinueBB];
        Ops << MkId(MergeBBID) << MkId(ContinueBBID)
            << MkNum(spv::SelectionControlMaskNone);

        auto *MergeInst = new SPIRVInstruction(spv::OpLoopMerge, Ops);
        SPIRVInstList.insert(InsertPoint, MergeInst);

      } else if (Br->isConditional()) {
        bool HasBackEdge = false;

        for (unsigned i = 0; i < Br->getNumSuccessors(); i++) {
          if (LI.isLoopHeader(Br->getSuccessor(i))) {
            HasBackEdge = true;
          }
        }
        if (!HasBackEdge) {
          //
          // Generate OpSelectionMerge.
          //
          // Ops[0] = Merge Block ID
          // Ops[1] = Selection Control
          SPIRVOperandList Ops;

          // StructurizeCFG pass already manipulated CFG. Just use false block
          // of branch instruction as merge block.
          uint32_t MergeBBID = VMap[Br->getSuccessor(1)];
          Ops << MkId(MergeBBID) << MkNum(spv::SelectionControlMaskNone);

          auto *MergeInst = new SPIRVInstruction(spv::OpSelectionMerge, Ops);
          SPIRVInstList.insert(InsertPoint, MergeInst);
        }
      }

      if (Br->isConditional()) {
        //
        // Generate OpBranchConditional.
        //
        // Ops[0] = Condition ID
        // Ops[1] = True Label ID
        // Ops[2] = False Label ID
        // Ops[3] ... Ops[n] = Branch weights (Literal Number)
        SPIRVOperandList Ops;

        uint32_t CondID = VMap[Br->getCondition()];
        uint32_t TrueBBID = VMap[Br->getSuccessor(0)];
        uint32_t FalseBBID = VMap[Br->getSuccessor(1)];

        Ops << MkId(CondID) << MkId(TrueBBID) << MkId(FalseBBID);

        auto *BrInst = new SPIRVInstruction(spv::OpBranchConditional, Ops);
        SPIRVInstList.insert(InsertPoint, BrInst);
      } else {
        //
        // Generate OpBranch.
        //
        // Ops[0] = Target Label ID
        SPIRVOperandList Ops;

        uint32_t TargetID = VMap[Br->getSuccessor(0)];
        Ops << MkId(TargetID);

        SPIRVInstList.insert(InsertPoint,
                             new SPIRVInstruction(spv::OpBranch, Ops));
      }
    } else if (PHINode *PHI = dyn_cast<PHINode>(Inst)) {
      //
      // Generate OpPhi.
      //
      // Ops[0] = Result Type ID
      // Ops[1] ... Ops[n] = (Variable ID, Parent ID) pairs
      SPIRVOperandList Ops;

      Ops << MkId(lookupType(PHI->getType()));

      for (unsigned i = 0; i < PHI->getNumIncomingValues(); i++) {
        uint32_t VarID = VMap[PHI->getIncomingValue(i)];
        uint32_t ParentID = VMap[PHI->getIncomingBlock(i)];
        Ops << MkId(VarID) << MkId(ParentID);
      }

      SPIRVInstList.insert(
          InsertPoint,
          new SPIRVInstruction(spv::OpPhi, std::get<2>(*DeferredInst), Ops));
    } else if (CallInst *Call = dyn_cast<CallInst>(Inst)) {
      Function *Callee = Call->getCalledFunction();
      auto callee_name = Callee->getName();
      glsl::ExtInst EInst = getDirectOrIndirectExtInstEnum(callee_name);

      if (EInst) {
        uint32_t &ExtInstImportID = getOpExtInstImportID();

        //
        // Generate OpExtInst.
        //

        // Ops[0] = Result Type ID
        // Ops[1] = Set ID (OpExtInstImport ID)
        // Ops[2] = Instruction Number (Literal Number)
        // Ops[3] ... Ops[n] = Operand 1, ... , Operand n
        SPIRVOperandList Ops;

        Ops << MkId(lookupType(Call->getType())) << MkId(ExtInstImportID) << MkNum(EInst);

        FunctionType *CalleeFTy = cast<FunctionType>(Call->getFunctionType());
        for (unsigned i = 0; i < CalleeFTy->getNumParams(); i++) {
          Ops << MkId(VMap[Call->getOperand(i)]);
        }

        auto *ExtInst = new SPIRVInstruction(spv::OpExtInst,
                                             std::get<2>(*DeferredInst), Ops);
        SPIRVInstList.insert(InsertPoint, ExtInst);

        const auto IndirectExtInst = getIndirectExtInstEnum(callee_name);
        if (IndirectExtInst != kGlslExtInstBad) {
          // Generate one more instruction that uses the result of the extended
          // instruction.  Its result id is one more than the id of the
          // extended instruction.
          LLVMContext &Context =
              Call->getParent()->getParent()->getParent()->getContext();

          auto generate_extra_inst = [this, &Context, &Call, &DeferredInst,
                                      &VMap, &SPIRVInstList, &InsertPoint](
                                         spv::Op opcode, Constant *constant) {
            //
            // Generate instruction like:
            //   result = opcode constant <extinst-result>
            //
            // Ops[0] = Result Type ID
            // Ops[1] = Operand 0 ;; the constant, suitably splatted
            // Ops[2] = Operand 1 ;; the result of the extended instruction
            SPIRVOperandList Ops;

            Type *resultTy = Call->getType();
            Ops << MkId(lookupType(resultTy));

            if (auto *vectorTy = dyn_cast<VectorType>(resultTy)) {
              constant = ConstantVector::getSplat(
                  static_cast<unsigned>(vectorTy->getNumElements()), constant);
            }
            Ops << MkId(VMap[constant]) << MkId(std::get<2>(*DeferredInst));

            SPIRVInstList.insert(
                InsertPoint, new SPIRVInstruction(
                                 opcode, std::get<2>(*DeferredInst) + 1, Ops));
          };

          switch (IndirectExtInst) {
          case glsl::ExtInstFindUMsb: // Implementing clz
            generate_extra_inst(
                spv::OpISub, ConstantInt::get(Type::getInt32Ty(Context), 31));
            break;
          case glsl::ExtInstAcos:  // Implementing acospi
          case glsl::ExtInstAsin:  // Implementing asinpi
          case glsl::ExtInstAtan2: // Implementing atan2pi
            generate_extra_inst(
                spv::OpFMul,
                ConstantFP::get(Type::getFloatTy(Context), kOneOverPi));
            break;

          default:
            assert(false && "internally inconsistent");
          }
        }

      } else if (Callee->getName().equals("_Z8popcounti") ||
                 Callee->getName().equals("_Z8popcountj") ||
                 Callee->getName().equals("_Z8popcountDv2_i") ||
                 Callee->getName().equals("_Z8popcountDv3_i") ||
                 Callee->getName().equals("_Z8popcountDv4_i") ||
                 Callee->getName().equals("_Z8popcountDv2_j") ||
                 Callee->getName().equals("_Z8popcountDv3_j") ||
                 Callee->getName().equals("_Z8popcountDv4_j")) {
        //
        // Generate OpBitCount
        //
        // Ops[0] = Result Type ID
        // Ops[1] = Base ID
        SPIRVOperandList Ops;
        Ops << MkId(lookupType(Call->getType()))
            << MkId(VMap[Call->getOperand(0)]);

        SPIRVInstList.insert(
            InsertPoint, new SPIRVInstruction(spv::OpBitCount,
                                              std::get<2>(*DeferredInst), Ops));

      } else if (Callee->getName().startswith(kCompositeConstructFunctionPrefix)) {

        // Generate an OpCompositeConstruct
        SPIRVOperandList Ops;

        // The result type.
        Ops << MkId(lookupType(Call->getType()));

        for (Use &use : Call->arg_operands()) {
          Value *val = use.get();
          Ops << MkId(VMap[use.get()]);
        }

        SPIRVInstList.insert(
            InsertPoint, new SPIRVInstruction(spv::OpCompositeConstruct,
                                              std::get<2>(*DeferredInst), Ops));

      } else {
        //
        // Generate OpFunctionCall.
        //

        // Ops[0] = Result Type ID
        // Ops[1] = Callee Function ID
        // Ops[2] ... Ops[n] = Argument 0, ... , Argument n
        SPIRVOperandList Ops;

        Ops << MkId( lookupType(Call->getType()));

        uint32_t CalleeID = VMap[Callee];
        if (CalleeID == 0) {
          errs() << "Can't translate function call.  Missing builtin? "
                 << Callee->getName() << " in: " << *Call << "\n";
          // TODO(dneto): Can we error out?  Enabling this llvm_unreachable
          // causes an infinite loop.  Instead, go ahead and generate
          // the bad function call.  A validator will catch the 0-Id.
          // llvm_unreachable("Can't translate function call");
        }

        Ops << MkId(CalleeID);

        FunctionType *CalleeFTy = cast<FunctionType>(Call->getFunctionType());
        for (unsigned i = 0; i < CalleeFTy->getNumParams(); i++) {
          Ops << MkId(VMap[Call->getOperand(i)]);
        }

        auto *CallInst = new SPIRVInstruction(spv::OpFunctionCall,
                                              std::get<2>(*DeferredInst), Ops);
        SPIRVInstList.insert(InsertPoint, CallInst);
      }
    }
  }
}

void SPIRVProducerPass::HandleDeferredDecorations(const DataLayout &DL) {
  if (getTypesNeedingArrayStride().empty() && LocalArgs.empty()) {
    return;
  }

  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();

  // Find an iterator pointing just past the last decoration.
  bool seen_decorations = false;
  auto DecoInsertPoint =
      std::find_if(SPIRVInstList.begin(), SPIRVInstList.end(),
                   [&seen_decorations](SPIRVInstruction *Inst) -> bool {
                     const bool is_decoration =
                         Inst->getOpcode() == spv::OpDecorate ||
                         Inst->getOpcode() == spv::OpMemberDecorate;
                     if (is_decoration) {
                       seen_decorations = true;
                       return false;
                     } else {
                       return seen_decorations;
                     }
                   });

  // Insert ArrayStride decorations on pointer types, due to OpPtrAccessChain
  // instructions we generated earlier.
  for (auto *type : getTypesNeedingArrayStride()) {
    Type *elemTy = nullptr;
    if (auto *ptrTy = dyn_cast<PointerType>(type)) {
      elemTy = ptrTy->getElementType();
    } else if (auto* arrayTy = dyn_cast<ArrayType>(type)) {
      elemTy = arrayTy->getArrayElementType();
    } else if (auto* seqTy = dyn_cast<SequentialType>(type)) {
      elemTy = seqTy->getSequentialElementType();
    } else {
      errs() << "Unhandled strided type " << *type << "\n";
      llvm_unreachable("Unhandled strided type");
    }

    // Ops[0] = Target ID
    // Ops[1] = Decoration (ArrayStride)
    // Ops[2] = Stride number (Literal Number)
    SPIRVOperandList Ops;

    // Same as DL.getIndexedOffsetInType( elemTy, { 1 } );
    const uint32_t stride = static_cast<uint32_t>(DL.getTypeAllocSize(elemTy));

    Ops << MkId(lookupType(type)) << MkNum(spv::DecorationArrayStride)
        << MkNum(stride);

    auto *DecoInst = new SPIRVInstruction(spv::OpDecorate, Ops);
    SPIRVInstList.insert(DecoInsertPoint, DecoInst);
  }

  // Emit SpecId decorations targeting the array size value.
  for (const Argument *arg : LocalArgs) {
    const LocalArgInfo &arg_info = LocalArgMap[arg];
    SPIRVOperandList Ops;
    Ops << MkId(arg_info.array_size_id) << MkNum(spv::DecorationSpecId)
        << MkNum(arg_info.spec_id);
    SPIRVInstList.insert(DecoInsertPoint,
                         new SPIRVInstruction(spv::OpDecorate, Ops));
  }
}

glsl::ExtInst SPIRVProducerPass::getExtInstEnum(StringRef Name) {
  return StringSwitch<glsl::ExtInst>(Name)
      .Case("_Z3absi", glsl::ExtInst::ExtInstSAbs)
      .Case("_Z3absDv2_i", glsl::ExtInst::ExtInstSAbs)
      .Case("_Z3absDv3_i", glsl::ExtInst::ExtInstSAbs)
      .Case("_Z3absDv4_i", glsl::ExtInst::ExtInstSAbs)
      .Case("_Z5clampiii", glsl::ExtInst::ExtInstSClamp)
      .Case("_Z5clampDv2_iS_S_", glsl::ExtInst::ExtInstSClamp)
      .Case("_Z5clampDv3_iS_S_", glsl::ExtInst::ExtInstSClamp)
      .Case("_Z5clampDv4_iS_S_", glsl::ExtInst::ExtInstSClamp)
      .Case("_Z5clampjjj", glsl::ExtInst::ExtInstUClamp)
      .Case("_Z5clampDv2_jS_S_", glsl::ExtInst::ExtInstUClamp)
      .Case("_Z5clampDv3_jS_S_", glsl::ExtInst::ExtInstUClamp)
      .Case("_Z5clampDv4_jS_S_", glsl::ExtInst::ExtInstUClamp)
      .Case("_Z5clampfff", glsl::ExtInst::ExtInstFClamp)
      .Case("_Z5clampDv2_fS_S_", glsl::ExtInst::ExtInstFClamp)
      .Case("_Z5clampDv3_fS_S_", glsl::ExtInst::ExtInstFClamp)
      .Case("_Z5clampDv4_fS_S_", glsl::ExtInst::ExtInstFClamp)
      .Case("_Z3maxii", glsl::ExtInst::ExtInstSMax)
      .Case("_Z3maxDv2_iS_", glsl::ExtInst::ExtInstSMax)
      .Case("_Z3maxDv3_iS_", glsl::ExtInst::ExtInstSMax)
      .Case("_Z3maxDv4_iS_", glsl::ExtInst::ExtInstSMax)
      .Case("_Z3maxjj", glsl::ExtInst::ExtInstUMax)
      .Case("_Z3maxDv2_jS_", glsl::ExtInst::ExtInstUMax)
      .Case("_Z3maxDv3_jS_", glsl::ExtInst::ExtInstUMax)
      .Case("_Z3maxDv4_jS_", glsl::ExtInst::ExtInstUMax)
      .Case("_Z3maxff", glsl::ExtInst::ExtInstFMax)
      .Case("_Z3maxDv2_fS_", glsl::ExtInst::ExtInstFMax)
      .Case("_Z3maxDv3_fS_", glsl::ExtInst::ExtInstFMax)
      .Case("_Z3maxDv4_fS_", glsl::ExtInst::ExtInstFMax)
      .StartsWith("_Z4fmax", glsl::ExtInst::ExtInstFMax)
      .Case("_Z3minii", glsl::ExtInst::ExtInstSMin)
      .Case("_Z3minDv2_iS_", glsl::ExtInst::ExtInstSMin)
      .Case("_Z3minDv3_iS_", glsl::ExtInst::ExtInstSMin)
      .Case("_Z3minDv4_iS_", glsl::ExtInst::ExtInstSMin)
      .Case("_Z3minjj", glsl::ExtInst::ExtInstUMin)
      .Case("_Z3minDv2_jS_", glsl::ExtInst::ExtInstUMin)
      .Case("_Z3minDv3_jS_", glsl::ExtInst::ExtInstUMin)
      .Case("_Z3minDv4_jS_", glsl::ExtInst::ExtInstUMin)
      .Case("_Z3minff", glsl::ExtInst::ExtInstFMin)
      .Case("_Z3minDv2_fS_", glsl::ExtInst::ExtInstFMin)
      .Case("_Z3minDv3_fS_", glsl::ExtInst::ExtInstFMin)
      .Case("_Z3minDv4_fS_", glsl::ExtInst::ExtInstFMin)
      .StartsWith("_Z4fmin", glsl::ExtInst::ExtInstFMin)
      .StartsWith("_Z7degrees", glsl::ExtInst::ExtInstDegrees)
      .StartsWith("_Z7radians", glsl::ExtInst::ExtInstRadians)
      .StartsWith("_Z3mix", glsl::ExtInst::ExtInstFMix)
      .StartsWith("_Z4acos", glsl::ExtInst::ExtInstAcos)
      .StartsWith("_Z5acosh", glsl::ExtInst::ExtInstAcosh)
      .StartsWith("_Z4asin", glsl::ExtInst::ExtInstAsin)
      .StartsWith("_Z5asinh", glsl::ExtInst::ExtInstAsinh)
      .StartsWith("_Z4atan", glsl::ExtInst::ExtInstAtan)
      .StartsWith("_Z5atan2", glsl::ExtInst::ExtInstAtan2)
      .StartsWith("_Z5atanh", glsl::ExtInst::ExtInstAtanh)
      .StartsWith("_Z4ceil", glsl::ExtInst::ExtInstCeil)
      .StartsWith("_Z3sin", glsl::ExtInst::ExtInstSin)
      .StartsWith("_Z4sinh", glsl::ExtInst::ExtInstSinh)
      .StartsWith("_Z8half_sin", glsl::ExtInst::ExtInstSin)
      .StartsWith("_Z10native_sin", glsl::ExtInst::ExtInstSin)
      .StartsWith("_Z3cos", glsl::ExtInst::ExtInstCos)
      .StartsWith("_Z4cosh", glsl::ExtInst::ExtInstCosh)
      .StartsWith("_Z8half_cos", glsl::ExtInst::ExtInstCos)
      .StartsWith("_Z10native_cos", glsl::ExtInst::ExtInstCos)
      .StartsWith("_Z3tan", glsl::ExtInst::ExtInstTan)
      .StartsWith("_Z4tanh", glsl::ExtInst::ExtInstTanh)
      .StartsWith("_Z8half_tan", glsl::ExtInst::ExtInstTan)
      .StartsWith("_Z10native_tan", glsl::ExtInst::ExtInstTan)
      .StartsWith("_Z3exp", glsl::ExtInst::ExtInstExp)
      .StartsWith("_Z8half_exp", glsl::ExtInst::ExtInstExp)
      .StartsWith("_Z10native_exp", glsl::ExtInst::ExtInstExp)
      .StartsWith("_Z4exp2", glsl::ExtInst::ExtInstExp2)
      .StartsWith("_Z9half_exp2", glsl::ExtInst::ExtInstExp2)
      .StartsWith("_Z11native_exp2", glsl::ExtInst::ExtInstExp2)
      .StartsWith("_Z3log", glsl::ExtInst::ExtInstLog)
      .StartsWith("_Z8half_log", glsl::ExtInst::ExtInstLog)
      .StartsWith("_Z10native_log", glsl::ExtInst::ExtInstLog)
      .StartsWith("_Z4log2", glsl::ExtInst::ExtInstLog2)
      .StartsWith("_Z9half_log2", glsl::ExtInst::ExtInstLog2)
      .StartsWith("_Z11native_log2", glsl::ExtInst::ExtInstLog2)
      .StartsWith("_Z4fabs", glsl::ExtInst::ExtInstFAbs)
      .StartsWith("_Z5floor", glsl::ExtInst::ExtInstFloor)
      .StartsWith("_Z5ldexp", glsl::ExtInst::ExtInstLdexp)
      .StartsWith("_Z3pow", glsl::ExtInst::ExtInstPow)
      .StartsWith("_Z4powr", glsl::ExtInst::ExtInstPow)
      .StartsWith("_Z9half_powr", glsl::ExtInst::ExtInstPow)
      .StartsWith("_Z11native_powr", glsl::ExtInst::ExtInstPow)
      .StartsWith("_Z5round", glsl::ExtInst::ExtInstRound)
      .StartsWith("_Z4sqrt", glsl::ExtInst::ExtInstSqrt)
      .StartsWith("_Z9half_sqrt", glsl::ExtInst::ExtInstSqrt)
      .StartsWith("_Z11native_sqrt", glsl::ExtInst::ExtInstSqrt)
      .StartsWith("_Z5rsqrt", glsl::ExtInst::ExtInstInverseSqrt)
      .StartsWith("_Z10half_rsqrt", glsl::ExtInst::ExtInstInverseSqrt)
      .StartsWith("_Z12native_rsqrt", glsl::ExtInst::ExtInstInverseSqrt)
      .StartsWith("_Z5trunc", glsl::ExtInst::ExtInstTrunc)
      .StartsWith("_Z5frexp", glsl::ExtInst::ExtInstFrexp)
      .StartsWith("_Z4sign", glsl::ExtInst::ExtInstFSign)
      .StartsWith("_Z6length", glsl::ExtInst::ExtInstLength)
      .StartsWith("_Z8distance", glsl::ExtInst::ExtInstDistance)
      .StartsWith("_Z4step", glsl::ExtInst::ExtInstStep)
      .Case("_Z5crossDv3_fS_", glsl::ExtInst::ExtInstCross)
      .StartsWith("_Z9normalize", glsl::ExtInst::ExtInstNormalize)
      .StartsWith("llvm.fmuladd.", glsl::ExtInst::ExtInstFma)
      .Case("spirv.unpack.v2f16", glsl::ExtInst::ExtInstUnpackHalf2x16)
      .Case("spirv.pack.v2f16", glsl::ExtInst::ExtInstPackHalf2x16)
      .Case("clspv.fract.f", glsl::ExtInst::ExtInstFract)
      .Case("clspv.fract.v2f", glsl::ExtInst::ExtInstFract)
      .Case("clspv.fract.v3f", glsl::ExtInst::ExtInstFract)
      .Case("clspv.fract.v4f", glsl::ExtInst::ExtInstFract)
      .Default(kGlslExtInstBad);
}

glsl::ExtInst SPIRVProducerPass::getIndirectExtInstEnum(StringRef Name) {
  // Check indirect cases.
  return StringSwitch<glsl::ExtInst>(Name)
      .StartsWith("_Z3clz", glsl::ExtInst::ExtInstFindUMsb)
      // Use exact match on float arg because these need a multiply
      // of a constant of the right floating point type.
      .Case("_Z6acospif", glsl::ExtInst::ExtInstAcos)
      .Case("_Z6acospiDv2_f", glsl::ExtInst::ExtInstAcos)
      .Case("_Z6acospiDv3_f", glsl::ExtInst::ExtInstAcos)
      .Case("_Z6acospiDv4_f", glsl::ExtInst::ExtInstAcos)
      .Case("_Z6asinpif", glsl::ExtInst::ExtInstAsin)
      .Case("_Z6asinpiDv2_f", glsl::ExtInst::ExtInstAsin)
      .Case("_Z6asinpiDv3_f", glsl::ExtInst::ExtInstAsin)
      .Case("_Z6asinpiDv4_f", glsl::ExtInst::ExtInstAsin)
      .Case("_Z7atan2piff", glsl::ExtInst::ExtInstAtan2)
      .Case("_Z7atan2piDv2_fS_", glsl::ExtInst::ExtInstAtan2)
      .Case("_Z7atan2piDv3_fS_", glsl::ExtInst::ExtInstAtan2)
      .Case("_Z7atan2piDv4_fS_", glsl::ExtInst::ExtInstAtan2)
      .Default(kGlslExtInstBad);
}

glsl::ExtInst SPIRVProducerPass::getDirectOrIndirectExtInstEnum(StringRef Name) {
  auto direct = getExtInstEnum(Name);
  if (direct != kGlslExtInstBad)
    return direct;
  return getIndirectExtInstEnum(Name);
}

void SPIRVProducerPass::PrintResID(SPIRVInstruction *Inst) {
  out << "%" << Inst->getResultID();
}

void SPIRVProducerPass::PrintOpcode(SPIRVInstruction *Inst) {
  spv::Op Opcode = static_cast<spv::Op>(Inst->getOpcode());
  out << "\t" << spv::getOpName(Opcode);
}

void SPIRVProducerPass::PrintOperand(SPIRVOperand *Op) {
  SPIRVOperandType OpTy = Op->getType();
  switch (OpTy) {
  default: {
    llvm_unreachable("Unsupported SPIRV Operand Type???");
    break;
  }
  case SPIRVOperandType::NUMBERID: {
    out << "%" << Op->getNumID();
    break;
  }
  case SPIRVOperandType::LITERAL_STRING: {
    out << "\"" << Op->getLiteralStr() << "\"";
    break;
  }
  case SPIRVOperandType::LITERAL_INTEGER: {
    // TODO: Handle LiteralNum carefully.
    for (auto Word : Op->getLiteralNum()) {
      out << Word;
    }
    break;
  }
  case SPIRVOperandType::LITERAL_FLOAT: {
    // TODO: Handle LiteralNum carefully.
    for (auto Word : Op->getLiteralNum()) {
      APFloat APF = APFloat(APFloat::IEEEsingle(), APInt(32, Word));
      SmallString<8> Str;
      APF.toString(Str, 6, 2);
      out << Str;
    }
    break;
  }
  }
}

void SPIRVProducerPass::PrintCapability(SPIRVOperand *Op) {
  spv::Capability Cap = static_cast<spv::Capability>(Op->getNumID());
  out << spv::getCapabilityName(Cap);
}

void SPIRVProducerPass::PrintExtInst(SPIRVOperand *Op) {
  auto LiteralNum = Op->getLiteralNum();
  glsl::ExtInst Ext = static_cast<glsl::ExtInst>(LiteralNum[0]);
  out << glsl::getExtInstName(Ext);
}

void SPIRVProducerPass::PrintAddrModel(SPIRVOperand *Op) {
  spv::AddressingModel AddrModel =
      static_cast<spv::AddressingModel>(Op->getNumID());
  out << spv::getAddressingModelName(AddrModel);
}

void SPIRVProducerPass::PrintMemModel(SPIRVOperand *Op) {
  spv::MemoryModel MemModel = static_cast<spv::MemoryModel>(Op->getNumID());
  out << spv::getMemoryModelName(MemModel);
}

void SPIRVProducerPass::PrintExecModel(SPIRVOperand *Op) {
  spv::ExecutionModel ExecModel =
      static_cast<spv::ExecutionModel>(Op->getNumID());
  out << spv::getExecutionModelName(ExecModel);
}

void SPIRVProducerPass::PrintExecMode(SPIRVOperand *Op) {
  spv::ExecutionMode ExecMode = static_cast<spv::ExecutionMode>(Op->getNumID());
  out << spv::getExecutionModeName(ExecMode);
}

void SPIRVProducerPass::PrintSourceLanguage(SPIRVOperand *Op) {
  spv::SourceLanguage SourceLang = static_cast<spv::SourceLanguage>(Op->getNumID());
  out << spv::getSourceLanguageName(SourceLang);
}

void SPIRVProducerPass::PrintFuncCtrl(SPIRVOperand *Op) {
  spv::FunctionControlMask FuncCtrl =
      static_cast<spv::FunctionControlMask>(Op->getNumID());
  out << spv::getFunctionControlName(FuncCtrl);
}

void SPIRVProducerPass::PrintStorageClass(SPIRVOperand *Op) {
  spv::StorageClass StClass = static_cast<spv::StorageClass>(Op->getNumID());
  out << getStorageClassName(StClass);
}

void SPIRVProducerPass::PrintDecoration(SPIRVOperand *Op) {
  spv::Decoration Deco = static_cast<spv::Decoration>(Op->getNumID());
  out << getDecorationName(Deco);
}

void SPIRVProducerPass::PrintBuiltIn(SPIRVOperand *Op) {
  spv::BuiltIn BIn = static_cast<spv::BuiltIn>(Op->getNumID());
  out << getBuiltInName(BIn);
}

void SPIRVProducerPass::PrintSelectionControl(SPIRVOperand *Op) {
  spv::SelectionControlMask BIn =
      static_cast<spv::SelectionControlMask>(Op->getNumID());
  out << getSelectionControlName(BIn);
}

void SPIRVProducerPass::PrintLoopControl(SPIRVOperand *Op) {
  spv::LoopControlMask BIn = static_cast<spv::LoopControlMask>(Op->getNumID());
  out << getLoopControlName(BIn);
}

void SPIRVProducerPass::PrintDimensionality(SPIRVOperand *Op) {
  spv::Dim DIM = static_cast<spv::Dim>(Op->getNumID());
  out << getDimName(DIM);
}

void SPIRVProducerPass::PrintImageFormat(SPIRVOperand *Op) {
  spv::ImageFormat Format = static_cast<spv::ImageFormat>(Op->getNumID());
  out << getImageFormatName(Format);
}

void SPIRVProducerPass::PrintMemoryAccess(SPIRVOperand *Op) {
  out << spv::getMemoryAccessName(
      static_cast<spv::MemoryAccessMask>(Op->getNumID()));
}

void SPIRVProducerPass::PrintImageOperandsType(SPIRVOperand *Op) {
  auto LiteralNum = Op->getLiteralNum();
  spv::ImageOperandsMask Type =
      static_cast<spv::ImageOperandsMask>(LiteralNum[0]);
  out << getImageOperandsName(Type);
}

void SPIRVProducerPass::WriteSPIRVAssembly() {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();

  for (auto Inst : SPIRVInstList) {
    SPIRVOperandList Ops = Inst->getOperands();
    spv::Op Opcode = static_cast<spv::Op>(Inst->getOpcode());

    switch (Opcode) {
    default: {
      llvm_unreachable("Unsupported SPIRV instruction");
      break;
    }
    case spv::OpCapability: {
      // Ops[0] = Capability
      PrintOpcode(Inst);
      out << " ";
      PrintCapability(Ops[0]);
      out << "\n";
      break;
    }
    case spv::OpMemoryModel: {
      // Ops[0] = Addressing Model
      // Ops[1] = Memory Model
      PrintOpcode(Inst);
      out << " ";
      PrintAddrModel(Ops[0]);
      out << " ";
      PrintMemModel(Ops[1]);
      out << "\n";
      break;
    }
    case spv::OpEntryPoint: {
      // Ops[0] = Execution Model
      // Ops[1] = EntryPoint ID
      // Ops[2] = Name (Literal String)
      // Ops[3] ... Ops[n] = Interface ID
      PrintOpcode(Inst);
      out << " ";
      PrintExecModel(Ops[0]);
      for (uint32_t i = 1; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    case spv::OpExecutionMode: {
      // Ops[0] = Entry Point ID
      // Ops[1] = Execution Mode
      // Ops[2] ... Ops[n] = Optional literals according to Execution Mode
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintExecMode(Ops[1]);
      for (uint32_t i = 2; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    case spv::OpSource: {
      // Ops[0] = SourceLanguage ID
      // Ops[1] = Version (LiteralNum)
      PrintOpcode(Inst);
      out << " ";
      PrintSourceLanguage(Ops[0]);
      out << " ";
      PrintOperand(Ops[1]);
      out << "\n";
      break;
    }
    case spv::OpDecorate: {
      // Ops[0] = Target ID
      // Ops[1] = Decoration (Block or BufferBlock)
      // Ops[2] ... Ops[n] = Optional literals according to Decoration
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintDecoration(Ops[1]);
      // Handle BuiltIn OpDecorate specially.
      if (Ops[1]->getNumID() == spv::DecorationBuiltIn) {
        out << " ";
        PrintBuiltIn(Ops[2]);
      } else {
        for (uint32_t i = 2; i < Ops.size(); i++) {
          out << " ";
          PrintOperand(Ops[i]);
        }
      }
      out << "\n";
      break;
    }
    case spv::OpMemberDecorate: {
      // Ops[0] = Structure Type ID
      // Ops[1] = Member Index(Literal Number)
      // Ops[2] = Decoration
      // Ops[3] ... Ops[n] = Optional literals according to Decoration
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintOperand(Ops[1]);
      out << " ";
      PrintDecoration(Ops[2]);
      for (uint32_t i = 3; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    case spv::OpTypePointer: {
      // Ops[0] = Storage Class
      // Ops[1] = Element Type ID
      PrintResID(Inst);
      out << " = ";
      PrintOpcode(Inst);
      out << " ";
      PrintStorageClass(Ops[0]);
      out << " ";
      PrintOperand(Ops[1]);
      out << "\n";
      break;
    }
    case spv::OpTypeImage: {
      // Ops[0] = Sampled Type ID
      // Ops[1] = Dim ID
      // Ops[2] = Depth (Literal Number)
      // Ops[3] = Arrayed (Literal Number)
      // Ops[4] = MS (Literal Number)
      // Ops[5] = Sampled (Literal Number)
      // Ops[6] = Image Format ID
      PrintResID(Inst);
      out << " = ";
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintDimensionality(Ops[1]);
      out << " ";
      PrintOperand(Ops[2]);
      out << " ";
      PrintOperand(Ops[3]);
      out << " ";
      PrintOperand(Ops[4]);
      out << " ";
      PrintOperand(Ops[5]);
      out << " ";
      PrintImageFormat(Ops[6]);
      out << "\n";
      break;
    }
    case spv::OpFunction: {
      // Ops[0] : Result Type ID
      // Ops[1] : Function Control
      // Ops[2] : Function Type ID
      PrintResID(Inst);
      out << " = ";
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintFuncCtrl(Ops[1]);
      out << " ";
      PrintOperand(Ops[2]);
      out << "\n";
      break;
    }
    case spv::OpSelectionMerge: {
      // Ops[0] = Merge Block ID
      // Ops[1] = Selection Control
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintSelectionControl(Ops[1]);
      out << "\n";
      break;
    }
    case spv::OpLoopMerge: {
      // Ops[0] = Merge Block ID
      // Ops[1] = Continue Target ID
      // Ops[2] = Selection Control
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintOperand(Ops[1]);
      out << " ";
      PrintLoopControl(Ops[2]);
      out << "\n";
      break;
    }
    case spv::OpImageSampleExplicitLod: {
      // Ops[0] = Result Type ID
      // Ops[1] = Sampled Image ID
      // Ops[2] = Coordinate ID
      // Ops[3] = Image Operands Type ID
      // Ops[4] ... Ops[n] = Operands ID
      PrintResID(Inst);
      out << " = ";
      PrintOpcode(Inst);
      for (uint32_t i = 0; i < 3; i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << " ";
      PrintImageOperandsType(Ops[3]);
      for (uint32_t i = 4; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    case spv::OpVariable: {
      // Ops[0] : Result Type ID
      // Ops[1] : Storage Class
      // Ops[2] ... Ops[n] = Initializer IDs
      PrintResID(Inst);
      out << " = ";
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintStorageClass(Ops[1]);
      for (uint32_t i = 2; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    case spv::OpExtInst: {
      // Ops[0] = Result Type ID
      // Ops[1] = Set ID (OpExtInstImport ID)
      // Ops[2] = Instruction Number (Literal Number)
      // Ops[3] ... Ops[n] = Operand 1, ... , Operand n
      PrintResID(Inst);
      out << " = ";
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintOperand(Ops[1]);
      out << " ";
      PrintExtInst(Ops[2]);
      for (uint32_t i = 3; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    case spv::OpCopyMemory: {
      // Ops[0] = Addressing Model
      // Ops[1] = Memory Model
      PrintOpcode(Inst);
      out << " ";
      PrintOperand(Ops[0]);
      out << " ";
      PrintOperand(Ops[1]);
      out << " ";
      PrintMemoryAccess(Ops[2]);
      out << " ";
      PrintOperand(Ops[3]);
      out << "\n";
      break;
    }
    case spv::OpExtension:
    case spv::OpControlBarrier:
    case spv::OpMemoryBarrier:
    case spv::OpBranch:
    case spv::OpBranchConditional:
    case spv::OpStore:
    case spv::OpImageWrite:
    case spv::OpReturnValue:
    case spv::OpReturn:
    case spv::OpFunctionEnd: {
      PrintOpcode(Inst);
      for (uint32_t i = 0; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    case spv::OpExtInstImport:
    case spv::OpTypeRuntimeArray:
    case spv::OpTypeStruct:
    case spv::OpTypeSampler:
    case spv::OpTypeSampledImage:
    case spv::OpTypeInt:
    case spv::OpTypeFloat:
    case spv::OpTypeArray:
    case spv::OpTypeVector:
    case spv::OpTypeBool:
    case spv::OpTypeVoid:
    case spv::OpTypeFunction:
    case spv::OpFunctionParameter:
    case spv::OpLabel:
    case spv::OpPhi:
    case spv::OpLoad:
    case spv::OpSelect:
    case spv::OpAccessChain:
    case spv::OpPtrAccessChain:
    case spv::OpInBoundsAccessChain:
    case spv::OpUConvert:
    case spv::OpSConvert:
    case spv::OpConvertFToU:
    case spv::OpConvertFToS:
    case spv::OpConvertUToF:
    case spv::OpConvertSToF:
    case spv::OpFConvert:
    case spv::OpConvertPtrToU:
    case spv::OpConvertUToPtr:
    case spv::OpBitcast:
    case spv::OpIAdd:
    case spv::OpFAdd:
    case spv::OpISub:
    case spv::OpFSub:
    case spv::OpIMul:
    case spv::OpFMul:
    case spv::OpUDiv:
    case spv::OpSDiv:
    case spv::OpFDiv:
    case spv::OpUMod:
    case spv::OpSRem:
    case spv::OpFRem:
    case spv::OpBitwiseOr:
    case spv::OpBitwiseXor:
    case spv::OpBitwiseAnd:
    case spv::OpNot:
    case spv::OpShiftLeftLogical:
    case spv::OpShiftRightLogical:
    case spv::OpShiftRightArithmetic:
    case spv::OpBitCount:
    case spv::OpCompositeConstruct:
    case spv::OpCompositeExtract:
    case spv::OpVectorExtractDynamic:
    case spv::OpCompositeInsert:
    case spv::OpCopyObject:
    case spv::OpVectorInsertDynamic:
    case spv::OpVectorShuffle:
    case spv::OpIEqual:
    case spv::OpINotEqual:
    case spv::OpUGreaterThan:
    case spv::OpUGreaterThanEqual:
    case spv::OpULessThan:
    case spv::OpULessThanEqual:
    case spv::OpSGreaterThan:
    case spv::OpSGreaterThanEqual:
    case spv::OpSLessThan:
    case spv::OpSLessThanEqual:
    case spv::OpFOrdEqual:
    case spv::OpFOrdGreaterThan:
    case spv::OpFOrdGreaterThanEqual:
    case spv::OpFOrdLessThan:
    case spv::OpFOrdLessThanEqual:
    case spv::OpFOrdNotEqual:
    case spv::OpFUnordEqual:
    case spv::OpFUnordGreaterThan:
    case spv::OpFUnordGreaterThanEqual:
    case spv::OpFUnordLessThan:
    case spv::OpFUnordLessThanEqual:
    case spv::OpFUnordNotEqual:
    case spv::OpSampledImage:
    case spv::OpFunctionCall:
    case spv::OpConstantTrue:
    case spv::OpConstantFalse:
    case spv::OpConstant:
    case spv::OpSpecConstant:
    case spv::OpConstantComposite:
    case spv::OpSpecConstantComposite:
    case spv::OpConstantNull:
    case spv::OpLogicalOr:
    case spv::OpLogicalAnd:
    case spv::OpLogicalNot:
    case spv::OpLogicalNotEqual:
    case spv::OpUndef:
    case spv::OpIsInf:
    case spv::OpIsNan:
    case spv::OpAny:
    case spv::OpAll:
    case spv::OpImageQuerySize:
    case spv::OpAtomicIAdd:
    case spv::OpAtomicISub:
    case spv::OpAtomicExchange:
    case spv::OpAtomicIIncrement:
    case spv::OpAtomicIDecrement:
    case spv::OpAtomicCompareExchange:
    case spv::OpAtomicUMin:
    case spv::OpAtomicSMin:
    case spv::OpAtomicUMax:
    case spv::OpAtomicSMax:
    case spv::OpAtomicAnd:
    case spv::OpAtomicOr:
    case spv::OpAtomicXor:
    case spv::OpDot: {
      PrintResID(Inst);
      out << " = ";
      PrintOpcode(Inst);
      for (uint32_t i = 0; i < Ops.size(); i++) {
        out << " ";
        PrintOperand(Ops[i]);
      }
      out << "\n";
      break;
    }
    }
  }
}

void SPIRVProducerPass::WriteOneWord(uint32_t Word) {
  binaryOut->write(reinterpret_cast<const char *>(&Word), sizeof(uint32_t));
}

void SPIRVProducerPass::WriteResultID(SPIRVInstruction *Inst) {
  WriteOneWord(Inst->getResultID());
}

void SPIRVProducerPass::WriteWordCountAndOpcode(SPIRVInstruction *Inst) {
  // High 16 bit : Word Count
  // Low 16 bit  : Opcode
  uint32_t Word = Inst->getOpcode();
  Word |= Inst->getWordCount() << 16;
  WriteOneWord(Word);
}

void SPIRVProducerPass::WriteOperand(SPIRVOperand *Op) {
  SPIRVOperandType OpTy = Op->getType();
  switch (OpTy) {
  default: {
    llvm_unreachable("Unsupported SPIRV Operand Type???");
    break;
  }
  case SPIRVOperandType::NUMBERID: {
    WriteOneWord(Op->getNumID());
    break;
  }
  case SPIRVOperandType::LITERAL_STRING: {
    std::string Str = Op->getLiteralStr();
    const char *Data = Str.c_str();
    size_t WordSize = Str.size() / 4;
    for (unsigned Idx = 0; Idx < WordSize; Idx++) {
      WriteOneWord(*reinterpret_cast<const uint32_t *>(&Data[4 * Idx]));
    }

    uint32_t Remainder = Str.size() % 4;
    uint32_t LastWord = 0;
    if (Remainder) {
      for (unsigned Idx = 0; Idx < Remainder; Idx++) {
        LastWord |= Data[4 * WordSize + Idx] << 8 * Idx;
      }
    }

    WriteOneWord(LastWord);
    break;
  }
  case SPIRVOperandType::LITERAL_INTEGER:
  case SPIRVOperandType::LITERAL_FLOAT: {
    auto LiteralNum = Op->getLiteralNum();
    // TODO: Handle LiteranNum carefully.
    for (auto Word : LiteralNum) {
      WriteOneWord(Word);
    }
    break;
  }
  }
}

void SPIRVProducerPass::WriteSPIRVBinary() {
  SPIRVInstructionList &SPIRVInstList = getSPIRVInstList();

  for (auto Inst : SPIRVInstList) {
    SPIRVOperandList Ops{Inst->getOperands()};
    spv::Op Opcode = static_cast<spv::Op>(Inst->getOpcode());

    switch (Opcode) {
    default: {
      errs() << "Unsupported SPIR-V instruction opcode " << int(Opcode) << "\n";
      llvm_unreachable("Unsupported SPIRV instruction");
      break;
    }
    case spv::OpCapability:
    case spv::OpExtension:
    case spv::OpMemoryModel:
    case spv::OpEntryPoint:
    case spv::OpExecutionMode:
    case spv::OpSource:
    case spv::OpDecorate:
    case spv::OpMemberDecorate:
    case spv::OpBranch:
    case spv::OpBranchConditional:
    case spv::OpSelectionMerge:
    case spv::OpLoopMerge:
    case spv::OpStore:
    case spv::OpImageWrite:
    case spv::OpReturnValue:
    case spv::OpControlBarrier:
    case spv::OpMemoryBarrier:
    case spv::OpReturn:
    case spv::OpFunctionEnd:
    case spv::OpCopyMemory: {
      WriteWordCountAndOpcode(Inst);
      for (uint32_t i = 0; i < Ops.size(); i++) {
        WriteOperand(Ops[i]);
      }
      break;
    }
    case spv::OpTypeBool:
    case spv::OpTypeVoid:
    case spv::OpTypeSampler:
    case spv::OpLabel:
    case spv::OpExtInstImport:
    case spv::OpTypePointer:
    case spv::OpTypeRuntimeArray:
    case spv::OpTypeStruct:
    case spv::OpTypeImage:
    case spv::OpTypeSampledImage:
    case spv::OpTypeInt:
    case spv::OpTypeFloat:
    case spv::OpTypeArray:
    case spv::OpTypeVector:
    case spv::OpTypeFunction: {
      WriteWordCountAndOpcode(Inst);
      WriteResultID(Inst);
      for (uint32_t i = 0; i < Ops.size(); i++) {
        WriteOperand(Ops[i]);
      }
      break;
    }
    case spv::OpFunction:
    case spv::OpFunctionParameter:
    case spv::OpAccessChain:
    case spv::OpPtrAccessChain:
    case spv::OpInBoundsAccessChain:
    case spv::OpUConvert:
    case spv::OpSConvert:
    case spv::OpConvertFToU:
    case spv::OpConvertFToS:
    case spv::OpConvertUToF:
    case spv::OpConvertSToF:
    case spv::OpFConvert:
    case spv::OpConvertPtrToU:
    case spv::OpConvertUToPtr:
    case spv::OpBitcast:
    case spv::OpIAdd:
    case spv::OpFAdd:
    case spv::OpISub:
    case spv::OpFSub:
    case spv::OpIMul:
    case spv::OpFMul:
    case spv::OpUDiv:
    case spv::OpSDiv:
    case spv::OpFDiv:
    case spv::OpUMod:
    case spv::OpSRem:
    case spv::OpFRem:
    case spv::OpBitwiseOr:
    case spv::OpBitwiseXor:
    case spv::OpBitwiseAnd:
    case spv::OpNot:
    case spv::OpShiftLeftLogical:
    case spv::OpShiftRightLogical:
    case spv::OpShiftRightArithmetic:
    case spv::OpBitCount:
    case spv::OpCompositeConstruct:
    case spv::OpCompositeExtract:
    case spv::OpVectorExtractDynamic:
    case spv::OpCompositeInsert:
    case spv::OpCopyObject:
    case spv::OpVectorInsertDynamic:
    case spv::OpVectorShuffle:
    case spv::OpIEqual:
    case spv::OpINotEqual:
    case spv::OpUGreaterThan:
    case spv::OpUGreaterThanEqual:
    case spv::OpULessThan:
    case spv::OpULessThanEqual:
    case spv::OpSGreaterThan:
    case spv::OpSGreaterThanEqual:
    case spv::OpSLessThan:
    case spv::OpSLessThanEqual:
    case spv::OpFOrdEqual:
    case spv::OpFOrdGreaterThan:
    case spv::OpFOrdGreaterThanEqual:
    case spv::OpFOrdLessThan:
    case spv::OpFOrdLessThanEqual:
    case spv::OpFOrdNotEqual:
    case spv::OpFUnordEqual:
    case spv::OpFUnordGreaterThan:
    case spv::OpFUnordGreaterThanEqual:
    case spv::OpFUnordLessThan:
    case spv::OpFUnordLessThanEqual:
    case spv::OpFUnordNotEqual:
    case spv::OpExtInst:
    case spv::OpIsInf:
    case spv::OpIsNan:
    case spv::OpAny:
    case spv::OpAll:
    case spv::OpUndef:
    case spv::OpConstantNull:
    case spv::OpLogicalOr:
    case spv::OpLogicalAnd:
    case spv::OpLogicalNot:
    case spv::OpLogicalNotEqual:
    case spv::OpConstantComposite:
    case spv::OpSpecConstantComposite:
    case spv::OpConstantTrue:
    case spv::OpConstantFalse:
    case spv::OpConstant:
    case spv::OpSpecConstant:
    case spv::OpVariable:
    case spv::OpFunctionCall:
    case spv::OpSampledImage:
    case spv::OpImageSampleExplicitLod:
    case spv::OpImageQuerySize:
    case spv::OpSelect:
    case spv::OpPhi:
    case spv::OpLoad:
    case spv::OpAtomicIAdd:
    case spv::OpAtomicISub:
    case spv::OpAtomicExchange:
    case spv::OpAtomicIIncrement:
    case spv::OpAtomicIDecrement:
    case spv::OpAtomicCompareExchange:
    case spv::OpAtomicUMin:
    case spv::OpAtomicSMin:
    case spv::OpAtomicUMax:
    case spv::OpAtomicSMax:
    case spv::OpAtomicAnd:
    case spv::OpAtomicOr:
    case spv::OpAtomicXor:
    case spv::OpDot: {
      WriteWordCountAndOpcode(Inst);
      WriteOperand(Ops[0]);
      WriteResultID(Inst);
      for (uint32_t i = 1; i < Ops.size(); i++) {
        WriteOperand(Ops[i]);
      }
      break;
    }
    }
  }
}
