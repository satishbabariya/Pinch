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
#include "mlir/IR/Value.h"
#include "Dialect.h"
#include "Passes.h"
#include "BorrowChecker.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/STLExtras.h"
#include "mlir/IR/StandardTypes.h"

#define DEBUG_TYPE "shape-inference"

using namespace mlir;
using namespace pinch;
using namespace std;

using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::makeArrayRef;
using llvm::ScopedHashTableScope;
using llvm::SmallVector;
using llvm::StringRef;
using llvm::Twine;

/// Include the auto-generated definitions for the shape inference interfaces.
#include "BorrowCheckerOpInterfaces.cpp.inc"

#define BORROW_CHECK(d, op, src) \
  if (!(d)->check((op), (src))) { return signalPassFailure(); }

namespace {

enum OwType {
  u32 = 0,
  ref,
  mut,
};

/// Ownership information for a particular variable
class Owner {
public:
  // name of this owning variable
  StringRef name;
  // if this is a reference, here is the data it refers to
  Owner *ref_to;
  // number of references leased out to our value
  int ref_count, mut_ref_count;
  OwType type;

  Owner(StringRef n, Owner *rt, OwType ty) {
    this->name = n;
    this->ref_count = 0;
    this->mut_ref_count = 0;
    this->ref_to = rt;
    this->type = ty;
  }

  Owner(StringRef n)
      : Owner(n, NULL, OwType::u32)
  {}

  // This is a big state machine that checks the rules for
  // each type of operation. 'this' is the destination
  //
  // src may be NULL
  bool check(mlir::Operation *op, Owner *src) {
    if (op->getName().getStringRef().equals("pinch.borrow")) {
      assert(src);
      llvm::dbgs() << "    Borrowing " << src->name << "\n";

      // fail if there is a mutable borrow already active
      if (src->mut_ref_count > 0) {
        op->emitError("Cannot borrow a shared reference while a "
                      "mutable reference is active");
        return false;
      }
      src->ref_count++;
    } else if (op->getName().getStringRef().equals("pinch.borrow_mut")) {
      assert(src);
      llvm::dbgs() << "    Mutably Borrowing " << src->name << "\n";

      // we need to be the only reference
      if (src->mut_ref_count > 0 || src->ref_count > 0) {
        op->emitError("Cannot borrow a shared reference while a "
                      "mutable reference is active");
        return false;
      }
      src->mut_ref_count++;
    }
    // else do nothing since it might be another dialect

    return true;
  }
};

/// The BorrowCheckerPass is a FunctionPass that performs static
/// checking of pointer rules
class BorrowCheckerPass : public mlir::FunctionPass<BorrowCheckerPass> {
public:
  void runOnFunction() override {
    auto f = getFunction();

    vector<Owner *> owners;
    llvm::ScopedHashTable<StringRef, Owner *> symbolTable;
    ScopedHashTableScope<StringRef, Owner *> var_scope(symbolTable);

    // Add function arguments since they will not be previously known
    // these will be in the same order as the args are
    auto fsrcs = f.getAttrOfType<ArrayAttr>("src");
    if (fsrcs) {
      auto argsrcs = fsrcs.getValue();
      vector<StringRef> srcs;
      for (auto itr = argsrcs.begin(); itr != argsrcs.end(); itr++) {
        llvm::dbgs() << "Found argument source " << (*itr).cast<StringAttr>().getValue() << "\n";
        srcs.push_back((*itr).cast<StringAttr>().getValue());
      }

      int i = 0;
      for (auto itr = f.args_begin(); itr != f.args_end(); itr++) {
        llvm::dbgs() << "Found argument " << (*itr).getType() << "\n";

        // TODO check mutable reference args
        Owner *ow = new Owner(srcs[i], NULL,
                             (*itr).getType().isa<MemRefType>() ? OwType::ref: OwType::u32);
        symbolTable.insert(srcs[i], ow);
        i++;
      }
    }

    printf("-- starting borrow checker --\n");

    // Populate the worklist with the operations that need shape inference:
    // these are operations that return a dynamic shape.
    llvm::SmallPtrSet<mlir::Operation *, 16> opWorklist;
    f.walk([&](mlir::Operation *op) {
      llvm::dbgs() << "Borrow checking " << op->getName() << "\n";

      // check the destination
      auto dstattr = op->getAttrOfType<StringAttr>("dst");
      if (!dstattr || dstattr.getValue() == "")
        return;

      auto dst = dstattr.getValue();
      if (!symbolTable.lookup(dst)) {
        // This must be a newly active variable
        Owner *ow = new Owner(dst);
        assert(ow);
        llvm::dbgs() << "    inserting into symbol table: " << dst << "\n";
        symbolTable.insert(dst, ow);
        owners.push_back(ow);
      }

      auto dstown = symbolTable.lookup(dst);
      assert(dstown);

      // Check the source
      auto srcattr = op->getAttrOfType<StringAttr>("src");
      if (srcattr && srcattr.getValue() != "") {
        auto src = srcattr.getValue();

        auto srcown = symbolTable.lookup(src);
        if (srcown) {
          BORROW_CHECK(dstown, op, srcown);
        } else {
          op->emitError("owning src not found: " + src);
          return signalPassFailure();
        }
      } else {
        BORROW_CHECK(dstown, op, NULL);
      }
    });

    for (auto itr = owners.begin(); itr != owners.end(); itr++) {
      Owner *ow = *itr;
      StringRef name = ow->name;
      llvm::dbgs() << "Found symbol " << name << "\n";
      delete ow;
    }
  }
};
} // end anonymous namespace

/// Create a Shape Inference pass.
std::unique_ptr<mlir::Pass> mlir::pinch::createBorrowCheckerPass() {
  return std::make_unique<BorrowCheckerPass>();
}