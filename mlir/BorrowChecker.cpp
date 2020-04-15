//===- BorrowCheckerPass.cpp - Shape Inference ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a Function level pass performing interprocedural
// propagation of array shapes through function specialization.
//
//===----------------------------------------------------------------------===//

#include "mlir/Pass/Pass.h"
#include "Dialect.h"
#include "Passes.h"
#include "BorrowChecker.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "shape-inference"

using namespace mlir;
using namespace pinch;

/// Include the auto-generated definitions for the shape inference interfaces.
#include "BorrowCheckerOpInterfaces.cpp.inc"

namespace {
/// The BorrowCheckerPass is a FunctionPass that performs intra-procedural
/// shape inference.
///
///    Algorithm:
///
///   1) 
///
class BorrowCheckerPass : public mlir::FunctionPass<BorrowCheckerPass> {
public:
  void runOnFunction() override {
    auto f = getFunction();
    printf("-- starting borrow checker --\n");

    // Populate the worklist with the operations that need shape inference:
    // these are operations that return a dynamic shape.
    llvm::SmallPtrSet<mlir::Operation *, 16> opWorklist;
    f.walk([&](mlir::Operation *op) {
      // do something
      llvm::dbgs() << "Borrow checking " << *op << "\n";
    });
  }
};
} // end anonymous namespace

/// Create a Shape Inference pass.
std::unique_ptr<mlir::Pass> mlir::pinch::createBorrowCheckerPass() {
  return std::make_unique<BorrowCheckerPass>();
}
