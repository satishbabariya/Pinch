//===- MLIRGen.cpp - MLIR Generation from a Toy AST -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a simple IR generation targeting MLIR from a Module AST
// for the Toy language.
//
//===----------------------------------------------------------------------===//

#include "MLIRGen.h"
#include "AST.h"
#include "Dialect.h"

#include "mlir/Analysis/Verifier.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/StandardTypes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/Support/raw_ostream.h"
#include <numeric>
#include "llvm/Support/Debug.h"

using namespace mlir::pinch;
using namespace pinch;

using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::makeArrayRef;
using llvm::ScopedHashTableScope;
using llvm::SmallVector;
using llvm::StringRef;
using llvm::Twine;

namespace {

/// Implementation of a simple MLIR emission from the Pinch AST.
///
/// This will emit operations that are specific to the Pinch language, preserving
/// the semantics of the language and (hopefully) allow to perform accurate
/// analysis and transformation based on these high level semantics.
class MLIRGenImpl {
public:
  MLIRGenImpl(mlir::MLIRContext &context) : builder(&context) {}

  /// Public API: convert the AST for a Pinch module (source file) to an MLIR
  /// Module operation.
  mlir::ModuleOp mlirGen(ModuleAST &moduleAST) {
    // We create an empty MLIR module and codegen functions one at a time and
    // add them to the module.
    theModule = mlir::ModuleOp::create(builder.getUnknownLoc());

    for (FunctionAST &F : moduleAST) {
      auto func = mlirGen(F);
      if (!func)
        return nullptr;
      theModule.push_back(func);
    }

    // Verify the module after we have finished constructing it, this will check
    // the structural properties of the IR and invoke any specific verifiers we
    // have on the Pinch operations.
    if (failed(mlir::verify(theModule))) {
      theModule.emitError("module verification error");
      return nullptr;
    }

    return theModule;
  }

private:
  /// A "module" matches a Pinch source file: containing a list of functions.
  mlir::ModuleOp theModule;

  /// The builder is a helper class to create IR inside a function. The builder
  /// is stateful, in particular it keeps an "insertion point": this is where
  /// the next operations will be introduced.
  mlir::OpBuilder builder;

  /// The symbol table maps a variable name to a value in the current scope.
  /// Entering a function creates a new scope, and the function arguments are
  /// added to the mapping. When the processing of a function is terminated, the
  /// scope is destroyed and the mappings created in this scope are dropped.
  llvm::ScopedHashTable<StringRef, mlir::Value> symbolTable;

  /// Map a reference to the type it points to
  llvm::ScopedHashTable<StringRef, mlir::Type> reftypeTable;

  std::vector<mlir::Value> vals_to_drop;

  /// Helper conversion for a Pinch AST location to an MLIR location.
  mlir::Location loc(Location loc) {
    return builder.getFileLineColLoc(builder.getIdentifier(*loc.file), loc.line,
                                     loc.col);
  }

  /// Declare a variable in the current scope, return success if the variable
  /// wasn't declared yet.
  mlir::LogicalResult declare(llvm::StringRef var, mlir::Value value) {
    if (symbolTable.count(var))
      return mlir::failure();
    symbolTable.insert(var, value);
    return mlir::success();
  }

  /// Create the prototype for an MLIR function with as many arguments as the
  /// provided Pinch AST prototype.
  mlir::FuncOp mlirGen(PrototypeAST &proto) {
    auto location = loc(proto.loc());

    // This is a generic function, the return type will be inferred later.
    // Arguments type are uniformly unranked tensors.
    std::vector<mlir::Type> arg_types;
    for (auto itr = proto.getArgs().begin(); itr != proto.getArgs().end(); itr++) {
      arg_types.push_back(getType((*itr)->getType(), location));
    }

    ArrayRef<mlir::Type> rt = llvm::None;
    if (proto.getReturnType().type == pinch::u32) {
      auto inttype = mlir::IntegerType::getChecked(
          32,
          mlir::IntegerType::SignednessSemantics::Unsigned,
          location);

      if (proto.getReturnType().is_ref) {
        // TODO is_mut_ref
        auto rtt =  mlir::MemRefType::get(makeArrayRef<int64_t>(1),
                                     inttype);
        rt = makeArrayRef<mlir::Type>(
            rtt
           );
        
      } else {
        rt = makeArrayRef<mlir::Type>(inttype);
      }
    }
    

    std::vector<mlir::Attribute> argnames;
    for (auto itr = proto.getArgs().begin(); itr != proto.getArgs().end(); itr++) {
      argnames.push_back(builder.getStringAttr((*itr)->getName()));
    }

    auto argnames_ref = builder.getArrayAttr(llvm::makeArrayRef(argnames));
    auto src = builder.getNamedAttr(StringRef("src"), argnames_ref);
    auto attrs = makeArrayRef(src);
    auto func_type = builder.getFunctionType(arg_types, rt);
    return mlir::FuncOp::create(location, proto.getName(), func_type, attrs);
  }

  /// Emit a new function and add it to the MLIR module.
  mlir::FuncOp mlirGen(FunctionAST &funcAST) {
    // Create a scope in the symbol table to hold variable declarations.
    ScopedHashTableScope<llvm::StringRef, mlir::Value> var_scope(symbolTable);
    ScopedHashTableScope<llvm::StringRef, mlir::Type> ref_scope(reftypeTable);
    vals_to_drop.clear();

    // Create an MLIR function for the given prototype.
    mlir::FuncOp function(mlirGen(*funcAST.getProto()));
    if (!function)
      return nullptr;

    // Let's start the body of the function now!
    // In MLIR the entry block of the function is special: it must have the same
    // argument list as the function itself.
    auto &entryBlock = *function.addEntryBlock();
    auto protoArgs = funcAST.getProto()->getArgs();

    // Declare all the function arguments in the symbol table.
    for (const auto &name_value :
         llvm::zip(protoArgs, entryBlock.getArguments())) {
      if (failed(declare(std::get<0>(name_value)->getName(),
                         std::get<1>(name_value))))
        return nullptr;

      if (std::get<1>(name_value).getType().isa<mlir::MemRefType>()) {
        reftypeTable.insert(std::get<0>(name_value)->getName(),
                            std::get<1>(name_value).getType().cast<mlir::MemRefType>().getElementType());
      } else if (std::get<1>(name_value).getType().isa<BoxType>()) {
        reftypeTable.insert(std::get<0>(name_value)->getName(),
                            std::get<1>(name_value).getType().cast<BoxType>().getElementType());

        // mark these boxes as droppable
        vals_to_drop.push_back(std::get<1>(name_value));
      }
    }

    // Set the insertion point in the builder to the beginning of the function
    // body, it will be used throughout the codegen to create operations in this
    // function.
    builder.setInsertionPointToStart(&entryBlock);

    // Emit the body of the function.
    if (mlir::failed(mlirGen(*funcAST.getBody()))) {
      function.erase();
      return nullptr;
    }

    // Implicitly return void if no return statement was emitted.
    // FIXME: we may fix the parser instead to always return the last expression
    // (this would possibly help the REPL case later)
    ReturnOp returnOp;
    if (!entryBlock.empty())
      returnOp = dyn_cast<ReturnOp>(entryBlock.back());
    if (!returnOp) {
      // drop any heap values first
      for (auto itr = vals_to_drop.begin(); itr != vals_to_drop.end(); itr++) {
        builder.create<DropOp>(loc(funcAST.getProto()->loc()), *itr);
      }

      builder.create<ReturnOp>(loc(funcAST.getProto()->loc()));
    }

    // If this function isn't main, then set the visibility to private.
    if (funcAST.getProto()->getName() != "main")
      function.setVisibility(mlir::FuncOp::Visibility::Private);

    return function;
  }

  /// Emit a binary operation
  mlir::Value mlirGen(BinaryExprAST &binop, StringRef dst) {
    // First emit the operations for each side of the operation before emitting
    // the operation itself. For example if the expression is `a + foo(a)`
    // 1) First it will visiting the LHS, which will return a reference to the
    //    value holding `a`. This value should have been emitted at declaration
    //    time and registered in the symbol table, so nothing would be
    //    codegen'd. If the value is not in the symbol table, an error has been
    //    emitted and nullptr is returned.
    // 2) Then the RHS is visited (recursively) and a call to `foo` is emitted
    //    and the result value is returned. If an error occurs we get a nullptr
    //    and propagate.
    //
    mlir::Value lhs = mlirGen(*binop.getLHS());
    if (!lhs)
      return nullptr;
    mlir::Value rhs = mlirGen(*binop.getRHS());
    if (!rhs)
      return nullptr;
    auto location = loc(binop.loc());

    // Derive the operation name from the binary operator. At the moment we only
    // support '+' and '*'.
    switch (binop.getOp()) {
    case '+':
      return builder.create<AddOp>(location, lhs, rhs, dst);
    case '*':
      return builder.create<MulOp>(location, lhs, rhs, dst);
    }

    emitError(location, "invalid binary operator '") << binop.getOp() << "'";
    return nullptr;
  }
  mlir::Value mlirGen(BinaryExprAST &binop) {
    auto d = StringRef("");
    return mlirGen(binop, d);
  }

  /// This is a reference to a variable in an expression. The variable is
  /// expected to have been declared and so should have a value in the symbol
  /// table, otherwise emit an error and return nullptr.
  mlir::Value mlirGen(VariableExprAST &expr) {
    if (auto variable = symbolTable.lookup(expr.getName()))
      return variable;

    emitError(loc(expr.loc()), "error: unknown variable '")
        << expr.getName() << "'";
    return nullptr;
  }

  mlir::Value mlirGen(VariableRefExprAST &expr, StringRef dst) {
    auto location = loc(expr.loc());

    if (auto variable = symbolTable.lookup(expr.getName())) {
      if (variable.getType().isa<mlir::MemRefType>()) {
        emitError(loc(expr.loc()), "error: cannot take reference of a reference '");
        return nullptr;
      }

      auto vartype = mlir::MemRefType::get(makeArrayRef<int64_t>(1),
                                           variable.getType());
      return builder.create<BorrowOp>(location,
                                      vartype,
                                      variable,
                                      expr.getName(),
                                      dst);
    }

    emitError(loc(expr.loc()), "error: referencing unknown variable '")
        << expr.getName() << "'";
    return nullptr;
  }
  mlir::Value mlirGen(VariableRefExprAST &expr) {
    return mlirGen(expr, StringRef(""));
  }

  mlir::Value mlirGen(VariableMutRefExprAST &expr, StringRef dst) {
    auto location = loc(expr.loc());

    if (auto variable = symbolTable.lookup(expr.getName())) {
      if (variable.getType().isa<mlir::MemRefType>()) {
        emitError(loc(expr.loc()), "error: cannot take reference of a reference '");
        return nullptr;
      }

      auto vartype = mlir::MemRefType::get(makeArrayRef<int64_t>(1),
                                           variable.getType());
      return builder.create<BorrowMutOp>(location,
                                         vartype,
                                         variable,
                                         expr.getName(),
                                         dst);
    }

    emitError(loc(expr.loc()), "error: referencing unknown variable '")
        << expr.getName() << "'";
    return nullptr;
  }
  mlir::Value mlirGen(VariableMutRefExprAST &expr) {
    return mlirGen(expr, StringRef(""));
  }

  mlir::Value mlirGen(DerefExprAST &expr, StringRef dst) {
    auto location = loc(expr.loc());

    if (auto variable = symbolTable.lookup(expr.getName())) {
      if (auto ty = reftypeTable.lookup(expr.getName())) {
        return builder.create<DerefOp>(location,
                                       ty,
                                       variable,
                                       expr.getName(),
                                       dst);
      }
    }

    emitError(loc(expr.loc()), "error: referencing unknown variable '")
        << expr.getName() << "'";
    return nullptr;
  }
  mlir::Value mlirGen(DerefExprAST &expr) {
    return mlirGen(expr, StringRef(""));
  }

  /// Emit a return operation. This will return failure if any generation fails.
  mlir::LogicalResult mlirGen(ReturnExprAST &ret) {
    auto location = loc(ret.loc());

    // 'return' takes an optional expression, handle that case here.
    mlir::Value expr = nullptr;
    StringRef src("");
    if (ret.getExpr().hasValue()) {
      StringRef sr("return");
      if (!(expr = mlirGen(*ret.getExpr().getValue(), sr)))
        return mlir::failure();

      // record the name of the variable supplying the return,
      // if there is one
      if ((*ret.getExpr())->getKind() == pinch::ExprAST::Expr_Var) {
        auto v = cast<VariableExprAST>(*ret.getExpr());
        src = v->getName();
      } else if((*ret.getExpr())->getKind() == pinch::ExprAST::Expr_VarRef) {
        // If the return value had an SSA generated for it, we need to check
        // the 'return' source.
        src = sr;
      } else if((*ret.getExpr())->getKind() == pinch::ExprAST::Expr_VarMutRef) {
        // see above
        src = sr;
      }
    }

    // Otherwise, this return operation has zero operands.
    builder.create<ReturnOp>(location,
                             expr ? makeArrayRef(expr)
                             : ArrayRef<mlir::Value>(),
                             src);
    return mlir::success();
  }

  /// Emit a call expression. It emits specific operations for the ``
  /// builtin. Other identifiers are assumed to be user-defined functions.
  mlir::Value mlirGen(CallExprAST &call, StringRef dst) {
    llvm::StringRef callee = call.getCallee();
    auto location = loc(call.loc());

    // Codegen the operands first.
    SmallVector<mlir::Value, 4> operands;
    for (auto &expr : call.getArgs()) {
      auto arg = mlirGen(*expr);
      if (!arg)
        return nullptr;

      // if this argument is a box, generate a move for it
      if (arg.getType().isa<BoxType>()) {
        auto src = cast<VariableExprAST>(*expr);
        arg = builder.create<MoveOp>(location,
                                     arg,
                                     src.getName(),
                                     StringRef(""));

        // now remove it from the list of droppables
        if (auto variable = symbolTable.lookup(src.getName())) {
          vals_to_drop.erase(std::remove(
                                 vals_to_drop.begin(), vals_to_drop.end(), variable),
                             vals_to_drop.end());
        }
      }

      operands.push_back(arg);
    }

    // Otherwise this is a call to a user-defined function. Calls to ser-defined
    // functions are mapped to a custom call that takes the callee name as an
    // attribute.
    return builder.create<GenericCallOp>(location, callee, operands, dst);
  }
  mlir::Value mlirGen(CallExprAST &call) {
    StringRef dst("");
    return mlirGen(call, dst);
  }

  /// Emit a print expression.
  mlir::LogicalResult mlirGen(PrintExprAST &call) {
    auto arg = mlirGen(*call.getArg());
    if (!arg)
      return mlir::failure();

    builder.create<PrintOp>(loc(call.loc()), arg);
    return mlir::success();
  }

  /// Emit a box allocation
  mlir::Value mlirGen(BoxExprAST &call, StringRef dst) {
    auto val = cast<NumberExprAST>(call.getArg())->getValue();

    auto nop = builder.create<BoxOp>(loc(call.loc()), dst, val);
    vals_to_drop.push_back(nop);
    return nop;
  }

  mlir::Value mlirGen(BoxExprAST &call) {
    StringRef dst("");
    return mlirGen(call, dst);
  }
  

  /// Emit a constant for a single number (FIXME: semantic? broadcast?)
  mlir::Value mlirGen(NumberExprAST &num) {
    StringRef name("");
    return builder.create<ConstantOp>(loc(num.loc()), name, num.getValue());
  }
  mlir::Value mlirGen(NumberExprAST &num, StringRef name) {
    return builder.create<ConstantOp>(loc(num.loc()), name, num.getValue());
  }

  /// Dispatch codegen for the right expression subclass using RTTI.
  mlir::Value mlirGen(ExprAST &expr) {
    switch (expr.getKind()) {
    case pinch::ExprAST::Expr_BinOp:
      return mlirGen(cast<BinaryExprAST>(expr));
    case pinch::ExprAST::Expr_Var:
      return mlirGen(cast<VariableExprAST>(expr));
    case pinch::ExprAST::Expr_VarRef:
      return mlirGen(cast<VariableRefExprAST>(expr));
    case pinch::ExprAST::Expr_VarMutRef:
      return mlirGen(cast<VariableMutRefExprAST>(expr));
    case pinch::ExprAST::Expr_Deref:
      return mlirGen(cast<DerefExprAST>(expr));
    case pinch::ExprAST::Expr_Box:
      return mlirGen(cast<BoxExprAST>(expr));
    case pinch::ExprAST::Expr_Call:
      return mlirGen(cast<CallExprAST>(expr));
    case pinch::ExprAST::Expr_Num:
      return mlirGen(cast<NumberExprAST>(expr));
    default:
      emitError(loc(expr.loc()))
          << "MLIR codegen encountered an unhandled expr kind '"
          << Twine(expr.getKind()) << "'";
      return nullptr;
    }
  }
  // Always use this one!
  // This version handles attribute generation
  mlir::Value mlirGen(ExprAST &expr, StringRef dst) {
    switch (expr.getKind()) {
    case pinch::ExprAST::Expr_BinOp:
      return mlirGen(cast<BinaryExprAST>(expr), dst);
    case pinch::ExprAST::Expr_Var:
      return mlirGen(cast<VariableExprAST>(expr));
    case pinch::ExprAST::Expr_VarRef:
      return mlirGen(cast<VariableRefExprAST>(expr), dst);
    case pinch::ExprAST::Expr_VarMutRef:
      return mlirGen(cast<VariableMutRefExprAST>(expr), dst);
    case pinch::ExprAST::Expr_Deref:
      return mlirGen(cast<DerefExprAST>(expr), dst);
    case pinch::ExprAST::Expr_Box:
      return mlirGen(cast<BoxExprAST>(expr), dst);
    case pinch::ExprAST::Expr_Call:
      return mlirGen(cast<CallExprAST>(expr), dst);
    case pinch::ExprAST::Expr_Num:
      return mlirGen(cast<NumberExprAST>(expr), dst);
    default:
      emitError(loc(expr.loc()))
          << "MLIR codegen encountered an unhandled expr kind '"
          << Twine(expr.getKind()) << "'";
      return nullptr;
    }
  }
  

  /// Handle a variable declaration, we'll codegen the expression that forms the
  /// initializer and record the value in the symbol table before returning it.
  /// Future expressions will be able to reference this variable through symbol
  /// table lookup.
  mlir::Value mlirGen(VarDeclExprAST &vardecl) {
    auto init = vardecl.getInitVal();
    if (!init) {
      emitError(loc(vardecl.loc()),
                "missing initializer in variable declaration");
      return nullptr;
    }

    mlir::Value value = mlirGen(*init, vardecl.getName());
    if (!value)
      return nullptr;

    if (init->getKind() == pinch::ExprAST::Expr_Var) {
      auto src = cast<VariableExprAST>(init);

      // Create a move operation since we are moving the
      // value from init to vardecl
      auto mop  = builder.create<MoveOp>(loc(vardecl.loc()),
                                         value,
                                         src->getName(),
                                         vardecl.getName());

      // Register the value in the symbol table, but
      // take care that the value we point at is our
      // newly minted move operation
      if (failed(declare(vardecl.getName(), mop)))
        return nullptr;

      return mop;
    }

    // Register the value in the symbol table.
    if (failed(declare(vardecl.getName(), value)))
      return nullptr;

    if (init->getKind() == pinch::ExprAST::Expr_VarRef) {
      auto expr = cast<VariableRefExprAST>(init);

      // make a mapping of what type we point to
      if (auto variable = symbolTable.lookup(expr->getName())) {
        reftypeTable.insert(vardecl.getName(), variable.getType());
      }
    } else if (init->getKind() == pinch::ExprAST::Expr_VarMutRef) {
      auto expr = cast<VariableMutRefExprAST>(init);

      // make a mapping of what type we point to
      if (auto variable = symbolTable.lookup(expr->getName())) {
        reftypeTable.insert(vardecl.getName(), variable.getType());
      }
    } else if (init->getKind() == pinch::ExprAST::Expr_Box) {
      // make a mapping of what type we point to
      reftypeTable.insert(vardecl.getName(),
                          builder.getIntegerType(32, false));
    }
    return value;
  }

  /// Codegen a list of expression, return failure if one of them hit an error.
  mlir::LogicalResult mlirGen(ExprASTList &blockAST) {
    ScopedHashTableScope<StringRef, mlir::Value> var_scope(symbolTable);
    ScopedHashTableScope<StringRef, mlir::Type> ref_scope(reftypeTable);
    for (auto &expr : blockAST) {
      // Specific handling for variable declarations, return statement, and
      // print. These can only appear in block list and not in nested
      // expressions.
      if (auto *vardecl = dyn_cast<VarDeclExprAST>(expr.get())) {
        if (!mlirGen(*vardecl))
          return mlir::failure();
        continue;
      }

      if (auto *ret = dyn_cast<ReturnExprAST>(expr.get())) {
        // drop any heap values first
        for (auto itr = vals_to_drop.begin(); itr != vals_to_drop.end(); itr++) {
          builder.create<DropOp>(loc(ret->loc()), *itr);
        }

        return mlirGen(*ret);
      }

      if (auto *print = dyn_cast<PrintExprAST>(expr.get())) {
        if (mlir::failed(mlirGen(*print)))
          return mlir::success();
        continue;
      }

      // Generic expression dispatch codegen.
      if (!mlirGen(*expr))
        return mlir::failure();
    }
    return mlir::success();
  }

  /// Build an MLIR type from a Pinch AST variable type (forward to the generic
  /// getType above).
  mlir::Type getType(const VarType &type, mlir::Location loc) {
    if (type.is_ref) {
      auto inttype =  mlir::IntegerType::getChecked(32,
                                                    mlir::IntegerType::SignednessSemantics::Unsigned,
                                                    loc);
      return mlir::MemRefType::get(makeArrayRef<int64_t>(1), inttype);
    } else if (type.type == Type::box) {
      return BoxType::get(loc);
    } else {
      return mlir::IntegerType::getChecked(32,
                                           mlir::IntegerType::SignednessSemantics::Unsigned,
                                           loc);
    }
  }
};

} // namespace

namespace pinch {

// The public API for codegen.
mlir::OwningModuleRef mlirGen(mlir::MLIRContext &context,
                              ModuleAST &moduleAST) {
  return MLIRGenImpl(context).mlirGen(moduleAST);
}

} // namespace pinch
