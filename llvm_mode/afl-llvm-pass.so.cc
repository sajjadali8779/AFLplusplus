/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   Copyright 2015, 2016 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.

 */

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <list>
#include <string>
#include <fstream>

#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/CFG.h"

using namespace llvm;

namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;
      AFLCoverage() : ModulePass(ID) {
        char* instWhiteListFilename = getenv("AFL_LLVM_WHITELIST");
        if (instWhiteListFilename) {
          std::string line;
          std::ifstream fileStream;
          fileStream.open(instWhiteListFilename);
          if (!fileStream)
            report_fatal_error("Unable to open AFL_LLVM_WHITELIST");
          getline(fileStream, line);
          while (fileStream) {
            myWhitelist.push_back(line);
            getline(fileStream, line);
          }
        }
      }

      bool runOnModule(Module &M) override;

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }

    protected:

      std::list<std::string> myWhitelist;

  };

}


char AFLCoverage::ID = 0;


bool AFLCoverage::runOnModule(Module &M) {

  LLVMContext &C = M.getContext();

  IntegerType *Int8Ty  = IntegerType::getInt8Ty(C);
  IntegerType *Int64Ty  = IntegerType::getInt64Ty(C);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass" VERSION cRST " by <lszekeres@google.com>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  GlobalVariable *AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

//  GlobalVariable *AFLPrevLoc = new GlobalVariable(
//      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
//      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */

  int inst_blocks = 0;

  for (auto &F : M)
    for (auto &BB : F) {

      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(&(*IP));
      
      if (!myWhitelist.empty()) {
          bool instrumentBlock = false;

          /* Get the current location using debug information.
           * For now, just instrument the block if we are not able
           * to determine our location. */
          DebugLoc Loc = IP->getDebugLoc();
          if ( Loc ) {
              DILocation *cDILoc = dyn_cast<DILocation>(Loc.getAsMDNode());

              unsigned int instLine = cDILoc->getLine();
              StringRef instFilename = cDILoc->getFilename();

              if (instFilename.str().empty()) {
                  /* If the original location is empty, try using the inlined location */
                  DILocation *oDILoc = cDILoc->getInlinedAt();
                  if (oDILoc) {
                      instFilename = oDILoc->getFilename();
                      instLine = oDILoc->getLine();
                  }
              }

              /* Continue only if we know where we actually are */
              if (!instFilename.str().empty()) {
                  for (std::list<std::string>::iterator it = myWhitelist.begin(); it != myWhitelist.end(); ++it) {
                      /* We don't check for filename equality here because
                       * filenames might actually be full paths. Instead we
                       * check that the actual filename ends in the filename
                       * specified in the list. */
                      if (instFilename.str().length() >= it->length()) {
                          if (instFilename.str().compare(instFilename.str().length() - it->length(), it->length(), *it) == 0) {
                              instrumentBlock = true;
                              break;
                          }
                      }
                  }
              }
          }

          /* Either we couldn't figure out our location or the location is
           * not whitelisted, so we skip instrumentation. */
          if (!instrumentBlock) continue;
      }


      // only instrument if this basic block is the destination of a previous
      // basic block that has multiple successors
      // this gets rid of ~5-10% of instrumentations that are unnecessary
      // result: a little more speed and less map pollution
      int more_than_one = -1;

      for (BasicBlock *Pred : predecessors(&BB)) {
        int count = 0;
        if (more_than_one == -1)
          more_than_one = 0;
        //fprintf(stderr, " %p=>", Pred);
        for (BasicBlock *Succ : successors(Pred)) {
          //if (count > 0)
          //  fprintf(stderr, "|");
          if (Succ != NULL) count++;
          //fprintf(stderr, "%p", Succ);
        }
        if (count > 1)
          more_than_one = 1;
      }
      //fprintf(stderr, " == %d\n", more_than_one);
      if (more_than_one != 1)
        continue;

      if (!BlockAddress::get(&F, &BB))
        continue;

/************
  CODE:

    // so far we dont care if the map overflows which would result in a crash

    short:
      map[map[0]++] = &rip;
      
    long:
      uint64_t rip_addr = "leaq (%rip), %rdx\n";
      idx = map[0];
      map[idx] = rip_addr;
      idx++;
      map[0] = idx;

***********/

      /* idx = map[0] */
      LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
      MapPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *MapPtrPtr = IRB.CreateGEP(MapPtr, ConstantInt::get(Int64Ty, 0));
      LoadInst *Counter = IRB.CreateLoad(Int64Ty, MapPtrPtr);
      Counter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      /* map[idx] = rip */
      Value *Shifted = IRB.CreateShl(Counter, ConstantInt::get(Int8Ty, 3));
      Value *MapPtrIdx = IRB.CreateGEP(MapPtr, Shifted);
      IRB.CreateStore(BlockAddress::get(&F, &BB), MapPtrIdx)->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      /* idx++ */
      Value *Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int64Ty, 1));

      /* map[0] = idx */
      IRB.CreateStore(Incr, MapPtrPtr)->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      inst_blocks++;

    }

  /* Say something nice. */

  if (!be_quiet) {

    if (!inst_blocks) WARNF("No instrumentation targets found.");
    else OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks, getenv("AFL_HARDEN") ? "hardened" :
             ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
              "ASAN/MSAN" : "non-hardened"), inst_ratio);

  }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}


static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
