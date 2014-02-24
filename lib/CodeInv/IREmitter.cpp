//===--- IREmitter - Emits IR from SDnodes ----------------------*- C++ -*-===//
//
//              Fracture: The Draper Decompiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class uses SDNodes and emits IR. It is intended to be extended by Target
// implementations who have special ISD legalization nodes.
//
// Author: Richard Carback (rtc1032) <rcarback@draper.com>
// Date: October 16, 2013
//===----------------------------------------------------------------------===//

#include "CodeInv/IREmitter.h"
#include "CodeInv/Decompiler.h"

using namespace llvm;

namespace fracture {

IREmitter::IREmitter(Decompiler *TheDec, raw_ostream &InfoOut,
  raw_ostream &ErrOut) : Infos(InfoOut), Errs(ErrOut) {
  Dec = TheDec;
  DAG = Dec->getCurrentDAG();
  IRB = new IRBuilder<>(getGlobalContext());
  RegMap.grow(Dec->getDisassembler()->getMCDirector()->getMCRegisterInfo(
     )->getNumRegs());
}

IREmitter::~IREmitter() {
  delete IRB;
}

void IREmitter::EmitIR(BasicBlock *BB, SDNode *CurNode,
    std::stack<SDNode *> &NodeStack, std::map<SDValue, Value*> OpMap) {
  // Who uses this node (so we can find the next node)
  for (SDNode::use_iterator I = CurNode->use_begin(), E = CurNode->use_end();
      I != E; ++I) {
    // Save any chain uses to the Nodestack (to guarantee they get evaluated)
    if (I.getUse().getValueType() == MVT::Other) {
      NodeStack.push(*I);
      continue;
    }
  }

  Value *IRVal = visit(CurNode);

  // When we hit the return instruction we save all the local registers to their
  // global equivalents and reset our memory structure.
  if (IRVal != NULL && ReturnInst::classof(IRVal)) {
    BasicBlock *IB = IRB->GetInsertBlock();
    BasicBlock::iterator IP = --IB->end(); // before Ret Instr
    IRBuilder<> TmpB(IB, IP);

    unsigned NumRegs =
     Dec->getDisassembler()->getMCDirector()->getMCRegisterInfo()->getNumRegs();
    for (unsigned int i = 1, e = NumRegs; i != e; ++i) {
      AllocaInst *RegAlloca = RegMap[i];
      if (RegAlloca == NULL) {
        continue;
      }
      StringRef RegName = RegAlloca->getName();

      Value *RegGbl = Dec->getModule()->getGlobalVariable(RegName);
      if (RegGbl == NULL) {
        errs() << "EmitIR return: Global register not declared but alloca'd!\n";
        continue;
      }

      Instruction *RegLoad = TmpB.CreateLoad(RegAlloca,
        getIndexedValueName(RegName));
      RegLoad->setDebugLoc(CurNode->getDebugLoc());
      Instruction *RegStore = TmpB.CreateStore(RegLoad, RegGbl);
      RegStore->setDebugLoc(CurNode->getDebugLoc());
    }

    // Reset Data Structures
    RegMap.clear();
    VisitMap.clear();
    BaseNames.clear();
    RegMap.grow(Dec->getDisassembler()->getMCDirector()->getMCRegisterInfo(
     )->getNumRegs());
  }
}

StringRef IREmitter::getIndexedValueName(StringRef BaseName) {
  const ValueSymbolTable &ST = Dec->getModule()->getValueSymbolTable();

  // In the common case, the name is not already in the symbol table.
  Value *V = ST.lookup(BaseName);
  if (V == NULL) {
    return BaseName;
  }

  // Otherwise, there is a naming conflict.  Rename this value.
  // FIXME: AFAIK this is never deallocated (memory leak). It should be free'd
  // after it gets added to the symbol table (which appears to do a copy as
  // indicated by the original code that stack allocated this variable).
  SmallString<256> *UniqueName =
    new SmallString<256>(BaseName.begin(), BaseName.end());
  unsigned Size = BaseName.size();

  // Add '_' as the last character when BaseName ends in a number
  if (BaseName[Size-1] <= '9' && BaseName[Size-1] >= '0') {
    UniqueName->resize(Size+1);
    (*UniqueName)[Size] = '_';
    Size++;
  }

  unsigned LastUnique = 0;
  while (1) {
    // Trim any suffix off and append the next number.
    UniqueName->resize(Size);
    raw_svector_ostream(*UniqueName) << ++LastUnique;

    // Try insert the vmap entry with this suffix.
    V = ST.lookup(*UniqueName);
    // FIXME: ^^ this lookup does not appear to be working on non-globals...
    // Temporary Fix: check if it has a basenames entry
    if (V == NULL && BaseNames[*UniqueName].empty()) {
      BaseNames[*UniqueName] = BaseName;
      return *UniqueName;
    }
  }
}

StringRef IREmitter::getBaseValueName(StringRef BaseName) {
  // Note: An alternate approach would be to pull the Symbol table and
  // do a string search, but this is much easier to implement.
  StringRef Res = BaseNames.lookup(BaseName);
  if (Res.empty()) {
    return BaseName;
  }
  return Res;
}

StringRef IREmitter::getInstructionName(const SDNode *N) {
  // Look for register name in CopyToReg user
  for (SDNode::use_iterator I = N->use_begin(), E = N->use_end(); I != E; ++I) {
    if (I->getOpcode() == ISD::CopyToReg) {
      return getIndexedValueName(
        visitRegister(I->getOperand(1).getNode())->getName());
    }
  }
  return StringRef();
}

Value* IREmitter::visit(const SDNode *N) {
  // Note: Extenders of this class should probably copy the following block
  // Also note, however, that it is up to the visit function wether to save to
  // the map, because these functions have the option to return null.
  if (VisitMap.find(N) != VisitMap.end()) {
    return VisitMap[N];
  }

  IRB->SetCurrentDebugLocation(N->getDebugLoc());

  DEBUG(Infos << "Visiting Node: ");
  DEBUG(N->print(Infos));
  DEBUG(Infos << "\n");

  switch (N->getOpcode()) {
    default:
      DEBUG(Infos << "Unknown SDNode: ");
      DEBUG(N->print(Infos));
      DEBUG(Infos << "\n");
      break;
    // Do nothing nodes
    case ISD::EntryToken:
    case ISD::HANDLENODE:
    case ISD::UNDEF:
      return NULL;
    case ISD::CopyFromReg:        return visitCopyFromReg(N);
    case ISD::CopyToReg:          return visitCopyToReg(N);
    case ISD::Constant:           return visitConstant(N);
    case ISD::TokenFactor:        return visitTokenFactor(N);
    case ISD::MERGE_VALUES:       return visitMERGE_VALUES(N);
    case ISD::ADD:                return visitADD(N);
    case ISD::SUB:                return visitSUB(N);
    case ISD::ADDC:               return visitADDC(N);
    case ISD::SUBC:               return visitSUBC(N);
    case ISD::ADDE:               return visitADDE(N);
    case ISD::SUBE:               return visitSUBE(N);
    case ISD::MUL:                return visitMUL(N);
    case ISD::SDIV:               return visitSDIV(N);
    case ISD::UDIV:               return visitUDIV(N);
    case ISD::SREM:               return visitSREM(N);
    case ISD::UREM:               return visitUREM(N);
    case ISD::MULHU:              return visitMULHU(N);
    case ISD::MULHS:              return visitMULHS(N);
    case ISD::SMUL_LOHI:          return visitSMUL_LOHI(N);
    case ISD::UMUL_LOHI:          return visitUMUL_LOHI(N);
    case ISD::SMULO:              return visitSMULO(N);
    case ISD::UMULO:              return visitUMULO(N);
    case ISD::SDIVREM:            return visitSDIVREM(N);
    case ISD::UDIVREM:            return visitUDIVREM(N);
    case ISD::AND:                return visitAND(N);
    case ISD::OR:                 return visitOR(N);
    case ISD::XOR:                return visitXOR(N);
    case ISD::SHL:                return visitSHL(N);
    case ISD::SRA:                return visitSRA(N);
    case ISD::SRL:                return visitSRL(N);
    case ISD::CTLZ:               return visitCTLZ(N);
    case ISD::CTLZ_ZERO_UNDEF:    return visitCTLZ_ZERO_UNDEF(N);
    case ISD::CTTZ:               return visitCTTZ(N);
    case ISD::CTTZ_ZERO_UNDEF:    return visitCTTZ_ZERO_UNDEF(N);
    case ISD::CTPOP:              return visitCTPOP(N);
    case ISD::SELECT:             return visitSELECT(N);
    case ISD::VSELECT:            return visitVSELECT(N);
    case ISD::SELECT_CC:          return visitSELECT_CC(N);
    case ISD::SETCC:              return visitSETCC(N);
    case ISD::SIGN_EXTEND:        return visitSIGN_EXTEND(N);
    case ISD::ZERO_EXTEND:        return visitZERO_EXTEND(N);
    case ISD::ANY_EXTEND:         return visitANY_EXTEND(N);
    case ISD::SIGN_EXTEND_INREG:  return visitSIGN_EXTEND_INREG(N);
    case ISD::TRUNCATE:           return visitTRUNCATE(N);
    case ISD::BITCAST:            return visitBITCAST(N);
    case ISD::BUILD_PAIR:         return visitBUILD_PAIR(N);
    case ISD::FADD:               return visitFADD(N);
    case ISD::FSUB:               return visitFSUB(N);
    case ISD::FMUL:               return visitFMUL(N);
    case ISD::FMA:                return visitFMA(N);
    case ISD::FDIV:               return visitFDIV(N);
    case ISD::FREM:               return visitFREM(N);
    case ISD::FCOPYSIGN:          return visitFCOPYSIGN(N);
    case ISD::SINT_TO_FP:         return visitSINT_TO_FP(N);
    case ISD::UINT_TO_FP:         return visitUINT_TO_FP(N);
    case ISD::FP_TO_SINT:         return visitFP_TO_SINT(N);
    case ISD::FP_TO_UINT:         return visitFP_TO_UINT(N);
    case ISD::FP_ROUND:           return visitFP_ROUND(N);
    case ISD::FP_ROUND_INREG:     return visitFP_ROUND_INREG(N);
    case ISD::FP_EXTEND:          return visitFP_EXTEND(N);
    case ISD::FNEG:               return visitFNEG(N);
    case ISD::FABS:               return visitFABS(N);
    case ISD::FFLOOR:             return visitFFLOOR(N);
    case ISD::FCEIL:              return visitFCEIL(N);
    case ISD::FTRUNC:             return visitFTRUNC(N);
    case ISD::BRCOND:             return visitBRCOND(N);
    case ISD::BR_CC:              return visitBR_CC(N);
    case ISD::LOAD:               return visitLOAD(N);
    case ISD::STORE:              return visitSTORE(N);
    case ISD::INSERT_VECTOR_ELT:  return visitINSERT_VECTOR_ELT(N);
    case ISD::EXTRACT_VECTOR_ELT: return visitEXTRACT_VECTOR_ELT(N);
    case ISD::BUILD_VECTOR:       return visitBUILD_VECTOR(N);
    case ISD::CONCAT_VECTORS:     return visitCONCAT_VECTORS(N);
    case ISD::EXTRACT_SUBVECTOR:  return visitEXTRACT_SUBVECTOR(N);
    case ISD::VECTOR_SHUFFLE:     return visitVECTOR_SHUFFLE(N);
  }
  return NULL;
}

Value* IREmitter::visitCopyFromReg(const SDNode *N) {
  // Operand 0 - Chain node (ignored)
  // Operand 1 - RegisterSDNode, a machine register. We create an alloca, which
  //             is typically removed in a mem2reg pass

  // Skip if the register is never used. This happens for %noreg registers.
  if (!N->hasAnyUseOfValue(0)) {
    return NULL;
  }

  Value *RegVal = visitRegister(N->getOperand(1).getNode());
  if (RegVal == NULL) {
    errs() << "visitCopyFromReg: Invalid Register!\n";
    return NULL;
  }

  StringRef Name = getIndexedValueName(RegVal->getName());
  Instruction* Res = IRB->CreateLoad(RegVal, Name);
  VisitMap[N] = Res;
  Res->setDebugLoc(N->getDebugLoc());
  return Res;
}

Value* IREmitter::visitCopyToReg(const SDNode *N) {
  // Operand 0 - Chain node (ignored)
  // Operand 1 - Register Destination
  // Operand 2 - Source
  Value *RegVal = visitRegister(N->getOperand(1).getNode());
  Value* V = visit(N->getOperand(2).getNode());

  if (V == NULL || RegVal == NULL) {
    errs() << "Null values on CopyToReg, skipping!\n";
    return NULL;
  }
  Instruction* Res = IRB->CreateStore(V, RegVal);
  VisitMap[N] = Res;
  Res->setDebugLoc(N->getDebugLoc());
  return Res;
}

Value* IREmitter::visitConstant(const SDNode *N) {
  if (const ConstantSDNode *CSDN = dyn_cast<ConstantSDNode>(N)) {
    Value *Res = Constant::getIntegerValue(
      N->getValueType(0).getTypeForEVT(getGlobalContext()),
      CSDN->getAPIntValue());
    VisitMap[N] = Res;
    return Res;
  } else {
    Errs << "Could not convert ISD::Constant to integer!\n";
  }
  return NULL;
}


Value* IREmitter::visitTokenFactor(const SDNode *N) { return NULL; }
Value* IREmitter::visitMERGE_VALUES(const SDNode *N) { return NULL; }

Value* IREmitter::visitADD(const SDNode *N) {
  // Operand 0 and 1 are values to add
  Value *Op0 = visit(N->getOperand(0).getNode());
  Value *Op1 = visit(N->getOperand(1).getNode());
  StringRef BaseName = getInstructionName(N);
  if (BaseName.empty()) {
    BaseName = getBaseValueName(Op0->getName());
  }
  if (BaseName.empty()) {
    BaseName = getBaseValueName(Op1->getName());
  }
  StringRef Name = getIndexedValueName(BaseName);
  Instruction *Res = dyn_cast<Instruction>(IRB->CreateAdd(Op0, Op1, Name));
  Res->setDebugLoc(N->getDebugLoc());
  VisitMap[N] = Res;
  return Res;
}

Value* IREmitter::visitSUB(const SDNode *N) {
  // Operand 0 and 1 are values to sub
  Value *Op0 = visit(N->getOperand(0).getNode());
  Value *Op1 = visit(N->getOperand(1).getNode());
  StringRef BaseName = getInstructionName(N);
  if (BaseName.empty()) {
    BaseName = getBaseValueName(Op0->getName());
  }
  if (BaseName.empty()) {
    BaseName = getBaseValueName(Op1->getName());
  }
  StringRef Name = getIndexedValueName(BaseName);
  Instruction *Res = dyn_cast<Instruction>(IRB->CreateSub(Op0, Op1, Name));
  Res->setDebugLoc(N->getDebugLoc());
  VisitMap[N] = Res;
  return Res;
}

Value* IREmitter::visitADDC(const SDNode *N) { return NULL; }
Value* IREmitter::visitSUBC(const SDNode *N) { return NULL; }
Value* IREmitter::visitADDE(const SDNode *N) { return NULL; }
Value* IREmitter::visitSUBE(const SDNode *N) { return NULL; }
Value* IREmitter::visitMUL(const SDNode *N) { return NULL; }
Value* IREmitter::visitSDIV(const SDNode *N) { return NULL; }
Value* IREmitter::visitUDIV(const SDNode *N) { return NULL; }
Value* IREmitter::visitSREM(const SDNode *N) { return NULL; }
Value* IREmitter::visitUREM(const SDNode *N) { return NULL; }
Value* IREmitter::visitMULHU(const SDNode *N) { return NULL; }
Value* IREmitter::visitMULHS(const SDNode *N) { return NULL; }
Value* IREmitter::visitSMUL_LOHI(const SDNode *N) { return NULL; }
Value* IREmitter::visitUMUL_LOHI(const SDNode *N) { return NULL; }
Value* IREmitter::visitSMULO(const SDNode *N) { return NULL; }
Value* IREmitter::visitUMULO(const SDNode *N) { return NULL; }
Value* IREmitter::visitSDIVREM(const SDNode *N) { return NULL; }
Value* IREmitter::visitUDIVREM(const SDNode *N) { return NULL; }
Value* IREmitter::visitAND(const SDNode *N) { return NULL; }
Value* IREmitter::visitOR(const SDNode *N) { return NULL; }
Value* IREmitter::visitXOR(const SDNode *N) { return NULL; }
Value* IREmitter::visitSHL(const SDNode *N) { return NULL; }
Value* IREmitter::visitSRA(const SDNode *N) { return NULL; }
Value* IREmitter::visitSRL(const SDNode *N) { return NULL; }
Value* IREmitter::visitCTLZ(const SDNode *N) { return NULL; }
Value* IREmitter::visitCTLZ_ZERO_UNDEF(const SDNode *N) { return NULL; }
Value* IREmitter::visitCTTZ(const SDNode *N) { return NULL; }
Value* IREmitter::visitCTTZ_ZERO_UNDEF(const SDNode *N) { return NULL; }
Value* IREmitter::visitCTPOP(const SDNode *N) { return NULL; }
Value* IREmitter::visitSELECT(const SDNode *N) { return NULL; }
Value* IREmitter::visitVSELECT(const SDNode *N) { return NULL; }
Value* IREmitter::visitSELECT_CC(const SDNode *N) { return NULL; }
Value* IREmitter::visitSETCC(const SDNode *N) { return NULL; }
Value* IREmitter::visitSIGN_EXTEND(const SDNode *N) { return NULL; }
Value* IREmitter::visitZERO_EXTEND(const SDNode *N) { return NULL; }
Value* IREmitter::visitANY_EXTEND(const SDNode *N) { return NULL; }
Value* IREmitter::visitSIGN_EXTEND_INREG(const SDNode *N) { return NULL; }
Value* IREmitter::visitTRUNCATE(const SDNode *N) { return NULL; }
Value* IREmitter::visitBITCAST(const SDNode *N) { return NULL; }
Value* IREmitter::visitBUILD_PAIR(const SDNode *N) { return NULL; }
Value* IREmitter::visitFADD(const SDNode *N) { return NULL; }
Value* IREmitter::visitFSUB(const SDNode *N) { return NULL; }
Value* IREmitter::visitFMUL(const SDNode *N) { return NULL; }
Value* IREmitter::visitFMA(const SDNode *N) { return NULL; }
Value* IREmitter::visitFDIV(const SDNode *N) { return NULL; }
Value* IREmitter::visitFREM(const SDNode *N) { return NULL; }
Value* IREmitter::visitFCOPYSIGN(const SDNode *N) { return NULL; }
Value* IREmitter::visitSINT_TO_FP(const SDNode *N) { return NULL; }
Value* IREmitter::visitUINT_TO_FP(const SDNode *N) { return NULL; }
Value* IREmitter::visitFP_TO_SINT(const SDNode *N) { return NULL; }
Value* IREmitter::visitFP_TO_UINT(const SDNode *N) { return NULL; }
Value* IREmitter::visitFP_ROUND(const SDNode *N) { return NULL; }
Value* IREmitter::visitFP_ROUND_INREG(const SDNode *N) { return NULL; }
Value* IREmitter::visitFP_EXTEND(const SDNode *N) { return NULL; }
Value* IREmitter::visitFNEG(const SDNode *N) { return NULL; }
Value* IREmitter::visitFABS(const SDNode *N) { return NULL; }
Value* IREmitter::visitFCEIL(const SDNode *N) { return NULL; }
Value* IREmitter::visitFTRUNC(const SDNode *N) { return NULL; }
Value* IREmitter::visitFFLOOR(const SDNode *N) { return NULL; }
Value* IREmitter::visitBRCOND(const SDNode *N) { return NULL; }
Value* IREmitter::visitBR_CC(const SDNode *N) { return NULL; }

Value* IREmitter::visitLOAD(const SDNode *N) { 
  // Operand 0 - Addr to load, should be a pointer
  // Operand 1 - undef (ignored)
  // Operand 2 - Chain (ignored)
  // outs() << N->getDebugLoc().getLine() << "<<<\n";
  Value *Addr = visit(N->getOperand(0).getNode());
  StringRef BaseName = getBaseValueName(Addr->getName());
  StringRef Name = getIndexedValueName(BaseName);

  if (!Addr->getType()->isPointerTy()) {
    Addr = IRB->CreateIntToPtr(Addr, Addr->getType()->getPointerTo(), Name);
    (dyn_cast<Instruction>(Addr))->setDebugLoc(N->getDebugLoc());
  }
  Name = getIndexedValueName(BaseName);
  Instruction *Res = IRB->CreateLoad(Addr, Name);
  Res->setDebugLoc(N->getDebugLoc());
  VisitMap[N] = Res;
  return Res;
}

Value* IREmitter::visitSTORE(const SDNode *N) {
  // Operand 0 - The Value to store, usually a register or Constant
  // Operand 1 - An address/register+offset, assuming addressing modes were
  //             implemented properly
  // Operand 2 - undef (ignored)
  // Operand 3 - Chain (ignored)
  Value* StoreVal = visit(N->getOperand(0).getNode());
  Value* Addr = visit(N->getOperand(1).getNode());
  StringRef Name = getIndexedValueName(getBaseValueName(Addr->getName()));

  if (!Addr->getType()->isPointerTy()) {
    Addr = IRB->CreateIntToPtr(Addr, Addr->getType()->getPointerTo(), Name);
    (dyn_cast<Instruction>(Addr))->setDebugLoc(N->getDebugLoc());
  }

  Instruction *Res = IRB->CreateStore(StoreVal, Addr);
  Res->setDebugLoc(N->getDebugLoc());
  VisitMap[N] = Res;
  return Res;
}

Value* IREmitter::visitINSERT_VECTOR_ELT(const SDNode *N) { return NULL; }
Value* IREmitter::visitEXTRACT_VECTOR_ELT(const SDNode *N) { return NULL; }
Value* IREmitter::visitBUILD_VECTOR(const SDNode *N) { return NULL; }
Value* IREmitter::visitCONCAT_VECTORS(const SDNode *N) { return NULL; }
Value* IREmitter::visitEXTRACT_SUBVECTOR(const SDNode *N) { return NULL; }
Value* IREmitter::visitVECTOR_SHUFFLE(const SDNode *N) { return NULL; }

Value* IREmitter::visitRegister(const SDNode *N) {
  const RegisterSDNode *R = dyn_cast<RegisterSDNode>(N);
  if (R == NULL) {
    errs() << "visitRegister with no register!?\n";
    return NULL;
  }

  AllocaInst *RegAlloca = RegMap[R->getReg()];
  if (RegAlloca == NULL) {
    // Regname is %regname when printed this way.
    std::string RegName;
    raw_string_ostream RP(RegName);
    RP << PrintReg(R->getReg(), DAG ? DAG->getTarget().getRegisterInfo() : 0);
    RegName = RP.str().substr(1, RegName.size());

    Type* Ty = R->getValueType(0).getTypeForEVT(getGlobalContext());

    Value *RegGbl = Dec->getModule()->getGlobalVariable(RegName);
    if (RegGbl == NULL) {
      Constant *Initializer = Constant::getNullValue(Ty);
      RegGbl = new GlobalVariable(*Dec->getModule(), // Module
                                   Ty,                // Type
                                   false,             // isConstant
                                   GlobalValue::ExternalLinkage,
                                   Initializer,
                                   RegName);
    }
    // Alloca's need to be entered in at the beginning of a function.
    Function *TheFunction = IRB->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
      TheFunction->getEntryBlock().begin());
    RegAlloca = TmpB.CreateAlloca(Ty, 0, RegName);
    RegAlloca->setDebugLoc(N->getDebugLoc());

    // Load from the Global and put it in the local. When we visit the ret
    // for the function, we should load from the local and store in global.
    Instruction *RegLoad = TmpB.CreateLoad(RegGbl,
      getIndexedValueName(RegName));
    RegLoad->setDebugLoc(N->getDebugLoc());
    Instruction *RegStore = TmpB.CreateStore(RegLoad, RegAlloca);
    RegStore->setDebugLoc(N->getDebugLoc());

    RegMap[R->getReg()] = RegAlloca;
  }
  return RegAlloca;
}

} // End namespace fracture
