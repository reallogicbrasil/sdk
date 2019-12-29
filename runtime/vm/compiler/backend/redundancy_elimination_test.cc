// Copyright (c) 2018, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/redundancy_elimination.h"

#include <functional>

#include "vm/compiler/backend/block_builder.h"
#include "vm/compiler/backend/il_printer.h"
#include "vm/compiler/backend/il_test_helper.h"
#include "vm/compiler/backend/inliner.h"
#include "vm/compiler/backend/loops.h"
#include "vm/compiler/backend/type_propagator.h"
#include "vm/compiler/compiler_pass.h"
#include "vm/compiler/frontend/bytecode_reader.h"
#include "vm/compiler/frontend/kernel_to_il.h"
#include "vm/compiler/jit/jit_call_specializer.h"
#include "vm/log.h"
#include "vm/object.h"
#include "vm/parser.h"
#include "vm/symbols.h"
#include "vm/unit_test.h"

namespace dart {

static void NoopNative(Dart_NativeArguments args) {}

static Dart_NativeFunction NoopNativeLookup(Dart_Handle name,
                                            int argument_count,
                                            bool* auto_setup_scope) {
  ASSERT(auto_setup_scope != nullptr);
  *auto_setup_scope = false;
  return reinterpret_cast<Dart_NativeFunction>(&NoopNative);
}

// Flatten all non-captured LocalVariables from the given scope and its children
// and siblings into the given array based on their environment index.
static void FlattenScopeIntoEnvironment(FlowGraph* graph,
                                        LocalScope* scope,
                                        GrowableArray<LocalVariable*>* env) {
  for (intptr_t i = 0; i < scope->num_variables(); i++) {
    auto var = scope->VariableAt(i);
    if (var->is_captured()) {
      continue;
    }

    auto index = graph->EnvIndex(var);
    env->EnsureLength(index + 1, nullptr);
    (*env)[index] = var;
  }

  if (scope->sibling() != nullptr) {
    FlattenScopeIntoEnvironment(graph, scope->sibling(), env);
  }
  if (scope->child() != nullptr) {
    FlattenScopeIntoEnvironment(graph, scope->child(), env);
  }
}

#if !defined(PRODUCT)
void PopulateEnvironmentFromBytecodeLocalVariables(
    const Function& function,
    FlowGraph* graph,
    GrowableArray<LocalVariable*>* env) {
  const auto& bytecode = Bytecode::Handle(function.bytecode());
  ASSERT(!bytecode.IsNull());

  kernel::BytecodeLocalVariablesIterator iter(Thread::Current()->zone(),
                                              bytecode);
  while (iter.MoveNext()) {
    if (iter.IsVariableDeclaration() && !iter.IsCaptured()) {
      LocalVariable* const var = new LocalVariable(
          TokenPosition::kNoSource, TokenPosition::kNoSource,
          String::ZoneHandle(graph->zone(), iter.Name()),
          AbstractType::ZoneHandle(graph->zone(), iter.Type()));
      if (iter.Index() < 0) {  // Parameter.
        var->set_index(VariableIndex(-iter.Index() - kKBCParamEndSlotFromFp));
      } else {
        var->set_index(VariableIndex(-iter.Index()));
      }
      const intptr_t env_index = graph->EnvIndex(var);
      env->EnsureLength(env_index + 1, nullptr);
      (*env)[env_index] = var;
    }
  }
}
#endif

// Run TryCatchAnalyzer optimization on the function foo from the given script
// and check that the only variables from the given list are synchronized
// on catch entry.
static void TryCatchOptimizerTest(
    Thread* thread,
    const char* script_chars,
    std::initializer_list<const char*> synchronized) {
  // Load the script and exercise the code once.
  const auto& root_library =
      Library::Handle(LoadTestScript(script_chars, &NoopNativeLookup));
  Invoke(root_library, "main");

  // Build the flow graph.
  std::initializer_list<CompilerPass::Id> passes = {
      CompilerPass::kComputeSSA,      CompilerPass::kTypePropagation,
      CompilerPass::kApplyICData,     CompilerPass::kSelectRepresentations,
      CompilerPass::kTypePropagation, CompilerPass::kCanonicalize,
  };
  const auto& function = Function::Handle(GetFunction(root_library, "foo"));
  TestPipeline pipeline(function, CompilerPass::kJIT);
  FlowGraph* graph = pipeline.RunPasses(passes);

  // Finally run TryCatchAnalyzer on the graph (in AOT mode).
  OptimizeCatchEntryStates(graph, /*is_aot=*/true);

  EXPECT_EQ(1, graph->graph_entry()->catch_entries().length());
  auto scope = graph->parsed_function().scope();

  GrowableArray<LocalVariable*> env;
  if (function.is_declared_in_bytecode()) {
#if defined(PRODUCT)
    // In product mode information about local variables is not retained in
    // bytecode, so we can't find variables by names.
    return;
#else
    PopulateEnvironmentFromBytecodeLocalVariables(function, graph, &env);
#endif
  } else {
    FlattenScopeIntoEnvironment(graph, scope, &env);
  }

  for (intptr_t i = 0; i < env.length(); i++) {
    bool found = false;
    for (auto name : synchronized) {
      if (env[i]->name().Equals(name)) {
        found = true;
        break;
      }
    }
    if (!found) {
      env[i] = nullptr;
    }
  }

  CatchBlockEntryInstr* catch_entry = graph->graph_entry()->catch_entries()[0];

  // We should only synchronize state for variables from the synchronized list.
  for (auto defn : *catch_entry->initial_definitions()) {
    if (ParameterInstr* param = defn->AsParameter()) {
      EXPECT(0 <= param->index() && param->index() < env.length());
      EXPECT(env[param->index()] != nullptr);
    }
  }
}

//
// Tests for TryCatchOptimizer.
//

ISOLATE_UNIT_TEST_CASE(TryCatchOptimizer_DeadParameterElimination_Simple1) {
  const char* script_chars = R"(
      dynamic blackhole([dynamic val]) native 'BlackholeNative';
      foo(int p) {
        var a = blackhole(), b = blackhole();
        try {
          blackhole([a, b]);
        } catch (e) {
          // nothing is used
        }
      }
      main() {
        foo(42);
      }
  )";

  TryCatchOptimizerTest(thread, script_chars, /*synchronized=*/{});
}

ISOLATE_UNIT_TEST_CASE(TryCatchOptimizer_DeadParameterElimination_Simple2) {
  const char* script_chars = R"(
      dynamic blackhole([dynamic val]) native 'BlackholeNative';
      foo(int p) {
        var a = blackhole(), b = blackhole();
        try {
          blackhole([a, b]);
        } catch (e) {
          // a should be synchronized
          blackhole(a);
        }
      }
      main() {
        foo(42);
      }
  )";

  TryCatchOptimizerTest(thread, script_chars, /*synchronized=*/{"a"});
}

ISOLATE_UNIT_TEST_CASE(TryCatchOptimizer_DeadParameterElimination_Cyclic1) {
  const char* script_chars = R"(
      dynamic blackhole([dynamic val]) native 'BlackholeNative';
      foo(int p) {
        var a = blackhole(), b;
        for (var i = 0; i < 42; i++) {
          b = blackhole();
          try {
            blackhole([a, b]);
          } catch (e) {
            // a and i should be synchronized
          }
        }
      }
      main() {
        foo(42);
      }
  )";

  TryCatchOptimizerTest(thread, script_chars, /*synchronized=*/{"a", "i"});
}

ISOLATE_UNIT_TEST_CASE(TryCatchOptimizer_DeadParameterElimination_Cyclic2) {
  const char* script_chars = R"(
      dynamic blackhole([dynamic val]) native 'BlackholeNative';
      foo(int p) {
        var a = blackhole(), b = blackhole();
        for (var i = 0; i < 42; i++) {
          try {
            blackhole([a, b]);
          } catch (e) {
            // a, b and i should be synchronized
          }
        }
      }
      main() {
        foo(42);
      }
  )";

  TryCatchOptimizerTest(thread, script_chars, /*synchronized=*/{"a", "b", "i"});
}

// LoadOptimizer tests

// This family of tests verifies behavior of load forwarding when alias for an
// allocation A is created by creating a redefinition for it and then
// letting redefinition escape.
static void TestAliasingViaRedefinition(
    Thread* thread,
    bool make_it_escape,
    std::function<Definition*(CompilerState* S, FlowGraph*, Definition*)>
        make_redefinition) {
  const char* script_chars = R"(
    dynamic blackhole([a, b, c, d, e, f]) native 'BlackholeNative';
    class K {
      var field;
    }
  )";
  const Library& lib =
      Library::Handle(LoadTestScript(script_chars, NoopNativeLookup));

  const Class& cls = Class::Handle(
      lib.LookupLocalClass(String::Handle(Symbols::New(thread, "K"))));
  const Error& err = Error::Handle(cls.EnsureIsFinalized(thread));
  EXPECT(err.IsNull());

  const Field& field = Field::Handle(
      cls.LookupField(String::Handle(Symbols::New(thread, "field"))));
  EXPECT(!field.IsNull());

  const Function& blackhole =
      Function::ZoneHandle(GetFunction(lib, "blackhole"));

  using compiler::BlockBuilder;
  CompilerState S(thread);
  FlowGraphBuilderHelper H;

  // We are going to build the following graph:
  //
  // B0[graph_entry]
  // B1[function_entry]:
  //   v0 <- AllocateObject(class K)
  //   v1 <- LoadField(v0, K.field)
  //   v2 <- make_redefinition(v0)
  //   PushArgument(v1)
  // #if make_it_escape
  //   PushArgument(v2)
  // #endif
  //   v3 <- StaticCall(blackhole, v1, v2)
  //   v4 <- LoadField(v2, K.field)
  //   Return v4

  auto b1 = H.flow_graph()->graph_entry()->normal_entry();
  AllocateObjectInstr* v0;
  LoadFieldInstr* v1;
  PushArgumentInstr* push_v1;
  LoadFieldInstr* v4;
  ReturnInstr* ret;

  {
    BlockBuilder builder(H.flow_graph(), b1);
    auto& slot = Slot::Get(field, &H.flow_graph()->parsed_function());
    v0 = builder.AddDefinition(new AllocateObjectInstr(
        TokenPosition::kNoSource, cls, new PushArgumentsArray(0)));
    v1 = builder.AddDefinition(
        new LoadFieldInstr(new Value(v0), slot, TokenPosition::kNoSource));
    auto v2 = builder.AddDefinition(make_redefinition(&S, H.flow_graph(), v0));
    auto args = new PushArgumentsArray(2);
    push_v1 = builder.AddInstruction(new PushArgumentInstr(new Value(v1)));
    args->Add(push_v1);
    if (make_it_escape) {
      auto push_v2 =
          builder.AddInstruction(new PushArgumentInstr(new Value(v2)));
      args->Add(push_v2);
    }
    builder.AddInstruction(new StaticCallInstr(
        TokenPosition::kNoSource, blackhole, 0, Array::empty_array(), args,
        S.GetNextDeoptId(), 0, ICData::RebindRule::kStatic));
    v4 = builder.AddDefinition(
        new LoadFieldInstr(new Value(v2), slot, TokenPosition::kNoSource));
    ret = builder.AddInstruction(new ReturnInstr(
        TokenPosition::kNoSource, new Value(v4), S.GetNextDeoptId()));
  }
  H.FinishGraph();
  DominatorBasedCSE::Optimize(H.flow_graph());

  if (make_it_escape) {
    // Allocation must be considered aliased.
    EXPECT_PROPERTY(v0, !it.Identity().IsNotAliased());
  } else {
    // Allocation must be considered not-aliased.
    EXPECT_PROPERTY(v0, it.Identity().IsNotAliased());
  }

  // v1 should have been removed from the graph and replaced with constant_null.
  EXPECT_PROPERTY(v1, it.next() == nullptr && it.previous() == nullptr);
  EXPECT_PROPERTY(push_v1,
                  it.value()->definition() == H.flow_graph()->constant_null());

  if (make_it_escape) {
    // v4 however should not be removed from the graph, because v0 escapes into
    // blackhole.
    EXPECT_PROPERTY(v4, it.next() != nullptr && it.previous() != nullptr);
    EXPECT_PROPERTY(ret, it.value()->definition() == v4);
  } else {
    // If v0 it not aliased then v4 should also be removed from the graph.
    EXPECT_PROPERTY(v4, it.next() == nullptr && it.previous() == nullptr);
    EXPECT_PROPERTY(
        ret, it.value()->definition() == H.flow_graph()->constant_null());
  }
}

static Definition* MakeCheckNull(CompilerState* S,
                                 FlowGraph* flow_graph,
                                 Definition* defn) {
  return new CheckNullInstr(new Value(defn), String::ZoneHandle(),
                            S->GetNextDeoptId(), TokenPosition::kNoSource);
}

static Definition* MakeRedefinition(CompilerState* S,
                                    FlowGraph* flow_graph,
                                    Definition* defn) {
  return new RedefinitionInstr(new Value(defn));
}

static Definition* MakeAssertAssignable(CompilerState* S,
                                        FlowGraph* flow_graph,
                                        Definition* defn) {
  return new AssertAssignableInstr(TokenPosition::kNoSource, new Value(defn),
                                   new Value(flow_graph->constant_null()),
                                   new Value(flow_graph->constant_null()),
                                   AbstractType::ZoneHandle(Type::ObjectType()),
                                   Symbols::Empty(), S->GetNextDeoptId(),
                                   NNBDMode::kLegacy);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_RedefinitionAliasing_CheckNull_NoEscape) {
  TestAliasingViaRedefinition(thread, /*make_it_escape=*/false, MakeCheckNull);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_RedefinitionAliasing_CheckNull_Escape) {
  TestAliasingViaRedefinition(thread, /*make_it_escape=*/true, MakeCheckNull);
}

ISOLATE_UNIT_TEST_CASE(
    LoadOptimizer_RedefinitionAliasing_Redefinition_NoEscape) {
  TestAliasingViaRedefinition(thread, /*make_it_escape=*/false,
                              MakeRedefinition);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_RedefinitionAliasing_Redefinition_Escape) {
  TestAliasingViaRedefinition(thread, /*make_it_escape=*/true,
                              MakeRedefinition);
}

ISOLATE_UNIT_TEST_CASE(
    LoadOptimizer_RedefinitionAliasing_AssertAssignable_NoEscape) {
  TestAliasingViaRedefinition(thread, /*make_it_escape=*/false,
                              MakeAssertAssignable);
}

ISOLATE_UNIT_TEST_CASE(
    LoadOptimizer_RedefinitionAliasing_AssertAssignable_Escape) {
  TestAliasingViaRedefinition(thread, /*make_it_escape=*/true,
                              MakeAssertAssignable);
}

// This family of tests verifies behavior of load forwarding when alias for an
// allocation A is created by storing it into another object B and then
// either loaded from it ([make_it_escape] is true) or object B itself
// escapes ([make_host_escape] is true).
// We insert redefinition for object B to check that use list traversal
// correctly discovers all loads and stores from B.
static void TestAliasingViaStore(
    Thread* thread,
    bool make_it_escape,
    bool make_host_escape,
    std::function<Definition*(CompilerState* S, FlowGraph*, Definition*)>
        make_redefinition) {
  const char* script_chars = R"(
    dynamic blackhole([a, b, c, d, e, f]) native 'BlackholeNative';
    class K {
      var field;
    }
  )";
  const Library& lib =
      Library::Handle(LoadTestScript(script_chars, NoopNativeLookup));

  const Class& cls = Class::Handle(
      lib.LookupLocalClass(String::Handle(Symbols::New(thread, "K"))));
  const Error& err = Error::Handle(cls.EnsureIsFinalized(thread));
  EXPECT(err.IsNull());

  const Field& field = Field::Handle(
      cls.LookupField(String::Handle(Symbols::New(thread, "field"))));
  EXPECT(!field.IsNull());

  const Function& blackhole =
      Function::ZoneHandle(GetFunction(lib, "blackhole"));

  using compiler::BlockBuilder;
  CompilerState S(thread);
  FlowGraphBuilderHelper H;

  // We are going to build the following graph:
  //
  // B0[graph_entry]
  // B1[function_entry]:
  //   v0 <- AllocateObject(class K)
  //   v5 <- AllocateObject(class K)
  // #if !make_host_escape
  //   StoreField(v5 . K.field = v0)
  // #endif
  //   v1 <- LoadField(v0, K.field)
  //   v2 <- REDEFINITION(v5)
  //   PushArgument(v1)
  // #if make_it_escape
  //   v6 <- LoadField(v2, K.field)
  //   PushArgument(v6)
  // #elif make_host_escape
  //   StoreField(v2 . K.field = v0)
  //   PushArgument(v5)
  // #endif
  //   v3 <- StaticCall(blackhole, v1, v6)
  //   v4 <- LoadField(v0, K.field)
  //   Return v4

  auto b1 = H.flow_graph()->graph_entry()->normal_entry();
  AllocateObjectInstr* v0;
  AllocateObjectInstr* v5;
  LoadFieldInstr* v1;
  PushArgumentInstr* push_v1;
  LoadFieldInstr* v4;
  ReturnInstr* ret;

  {
    BlockBuilder builder(H.flow_graph(), b1);
    auto& slot = Slot::Get(field, &H.flow_graph()->parsed_function());
    v0 = builder.AddDefinition(new AllocateObjectInstr(
        TokenPosition::kNoSource, cls, new PushArgumentsArray(0)));
    v5 = builder.AddDefinition(new AllocateObjectInstr(
        TokenPosition::kNoSource, cls, new PushArgumentsArray(0)));
    if (!make_host_escape) {
      builder.AddInstruction(new StoreInstanceFieldInstr(
          slot, new Value(v5), new Value(v0), kEmitStoreBarrier,
          TokenPosition::kNoSource));
    }
    v1 = builder.AddDefinition(
        new LoadFieldInstr(new Value(v0), slot, TokenPosition::kNoSource));
    auto v2 = builder.AddDefinition(make_redefinition(&S, H.flow_graph(), v5));
    push_v1 = builder.AddInstruction(new PushArgumentInstr(new Value(v1)));
    auto args = new PushArgumentsArray(2);
    args->Add(push_v1);
    if (make_it_escape) {
      auto v6 = builder.AddDefinition(
          new LoadFieldInstr(new Value(v2), slot, TokenPosition::kNoSource));
      auto push_v6 =
          builder.AddInstruction(new PushArgumentInstr(new Value(v6)));
      args->Add(push_v6);
    } else if (make_host_escape) {
      builder.AddInstruction(new StoreInstanceFieldInstr(
          slot, new Value(v2), new Value(v0), kEmitStoreBarrier,
          TokenPosition::kNoSource));
      args->Add(builder.AddInstruction(new PushArgumentInstr(new Value(v5))));
    }
    builder.AddInstruction(new StaticCallInstr(
        TokenPosition::kNoSource, blackhole, 0, Array::empty_array(), args,
        S.GetNextDeoptId(), 0, ICData::RebindRule::kStatic));
    v4 = builder.AddDefinition(
        new LoadFieldInstr(new Value(v0), slot, TokenPosition::kNoSource));
    ret = builder.AddInstruction(new ReturnInstr(
        TokenPosition::kNoSource, new Value(v4), S.GetNextDeoptId()));
  }
  H.FinishGraph();
  DominatorBasedCSE::Optimize(H.flow_graph());

  if (make_it_escape || make_host_escape) {
    // Allocation must be considered aliased.
    EXPECT_PROPERTY(v0, !it.Identity().IsNotAliased());
  } else {
    // Allocation must not be considered aliased.
    EXPECT_PROPERTY(v0, it.Identity().IsNotAliased());
  }

  if (make_host_escape) {
    EXPECT_PROPERTY(v5, !it.Identity().IsNotAliased());
  } else {
    EXPECT_PROPERTY(v5, it.Identity().IsNotAliased());
  }

  // v1 should have been removed from the graph and replaced with constant_null.
  EXPECT_PROPERTY(v1, it.next() == nullptr && it.previous() == nullptr);
  EXPECT_PROPERTY(push_v1,
                  it.value()->definition() == H.flow_graph()->constant_null());

  if (make_it_escape || make_host_escape) {
    // v4 however should not be removed from the graph, because v0 escapes into
    // blackhole.
    EXPECT_PROPERTY(v4, it.next() != nullptr && it.previous() != nullptr);
    EXPECT_PROPERTY(ret, it.value()->definition() == v4);
  } else {
    // If v0 it not aliased then v4 should also be removed from the graph.
    EXPECT_PROPERTY(v4, it.next() == nullptr && it.previous() == nullptr);
    EXPECT_PROPERTY(
        ret, it.value()->definition() == H.flow_graph()->constant_null());
  }
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaStore_CheckNull_NoEscape) {
  TestAliasingViaStore(thread, /*make_it_escape=*/false,
                       /* make_host_escape= */ false, MakeCheckNull);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaStore_CheckNull_Escape) {
  TestAliasingViaStore(thread, /*make_it_escape=*/true,
                       /* make_host_escape= */ false, MakeCheckNull);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaStore_CheckNull_EscapeViaHost) {
  TestAliasingViaStore(thread, /*make_it_escape=*/false,
                       /* make_host_escape= */ true, MakeCheckNull);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaStore_Redefinition_NoEscape) {
  TestAliasingViaStore(thread, /*make_it_escape=*/false,
                       /* make_host_escape= */ false, MakeRedefinition);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaStore_Redefinition_Escape) {
  TestAliasingViaStore(thread, /*make_it_escape=*/true,
                       /* make_host_escape= */ false, MakeRedefinition);
}

ISOLATE_UNIT_TEST_CASE(
    LoadOptimizer_AliasingViaStore_Redefinition_EscapeViaHost) {
  TestAliasingViaStore(thread, /*make_it_escape=*/false,
                       /* make_host_escape= */ true, MakeRedefinition);
}

ISOLATE_UNIT_TEST_CASE(
    LoadOptimizer_AliasingViaStore_AssertAssignable_NoEscape) {
  TestAliasingViaStore(thread, /*make_it_escape=*/false,
                       /* make_host_escape= */ false, MakeAssertAssignable);
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaStore_AssertAssignable_Escape) {
  TestAliasingViaStore(thread, /*make_it_escape=*/true,
                       /* make_host_escape= */ false, MakeAssertAssignable);
}

ISOLATE_UNIT_TEST_CASE(
    LoadOptimizer_AliasingViaStore_AssertAssignable_EscapeViaHost) {
  TestAliasingViaStore(thread, /*make_it_escape=*/false,
                       /* make_host_escape= */ true, MakeAssertAssignable);
}

// This test verifies behavior of load forwarding when an alias for an
// allocation A is created after forwarded due to an eliminated load. That is,
// allocation A is stored and later retrieved via load B, B is used in store C
// (with a different constant index/index_scale than in B but that overlaps),
// and then A is retrieved again (with the same index as in B) in load D.
//
// When B gets eliminated and replaced with in C and D with A, the store in C
// should stop the load D from being eliminated. This is a scenario that came
// up when forwarding typed data view factory arguments.
//
// Here, the entire scenario happens within a single basic block.
ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaLoadElimination_SingleBlock) {
  const char* kScript = R"(
    import 'dart:typed_data';

    testViewAliasing1() {
      final f64 = new Float64List(1);
      final f32 = new Float32List.view(f64.buffer);
      f64[0] = 1.0; // Should not be forwarded.
      f32[1] = 2.0; // upper 32bits for 2.0f and 2.0 are the same
      return f64[0];
    }
  )";

  const auto& root_library = Library::Handle(LoadTestScript(kScript));
  const auto& function =
      Function::Handle(GetFunction(root_library, "testViewAliasing1"));

  Invoke(root_library, "testViewAliasing1");

  TestPipeline pipeline(function, CompilerPass::kJIT);
  FlowGraph* flow_graph = pipeline.RunPasses({});

  auto entry = flow_graph->graph_entry()->normal_entry();
  EXPECT(entry != nullptr);

  StaticCallInstr* list_factory = nullptr;
  UnboxedConstantInstr* double_one = nullptr;
  StoreIndexedInstr* first_store = nullptr;
  StoreIndexedInstr* second_store = nullptr;
  LoadIndexedInstr* final_load = nullptr;
  BoxInstr* boxed_result = nullptr;

  ILMatcher cursor(flow_graph, entry);
  RELEASE_ASSERT(cursor.TryMatch(
      {
          {kMatchAndMoveStaticCall, &list_factory},
          {kMatchAndMoveUnboxedConstant, &double_one},
          {kMatchAndMoveStoreIndexed, &first_store},
          {kMatchAndMoveStoreIndexed, &second_store},
          {kMatchAndMoveLoadIndexed, &final_load},
          {kMatchAndMoveBox, &boxed_result},
          kMatchReturn,
      },
      /*insert_before=*/kMoveGlob));

  EXPECT(first_store->array()->definition() == list_factory);
  EXPECT(second_store->array()->definition() == list_factory);
  EXPECT(boxed_result->value()->definition() != double_one);
  EXPECT(boxed_result->value()->definition() == final_load);
}

// This test verifies behavior of load forwarding when an alias for an
// allocation A is created after forwarded due to an eliminated load. That is,
// allocation A is stored and later retrieved via load B, B is used in store C
// (with a different constant index/index_scale than in B but that overlaps),
// and then A is retrieved again (with the same index as in B) in load D.
//
// When B gets eliminated and replaced with in C and D with A, the store in C
// should stop the load D from being eliminated. This is a scenario that came
// up when forwarding typed data view factory arguments.
//
// Here, the scenario is split across basic blocks. This is a cut-down version
// of language_2/vm/load_to_load_forwarding_vm_test.dart with just enough extra
// to keep testViewAliasing1 from being optimized into a single basic block.
// Thus, this test may be brittler than the other, if future work causes it to
// end up compiled into a single basic block (or a simpler set of basic blocks).
ISOLATE_UNIT_TEST_CASE(LoadOptimizer_AliasingViaLoadElimination_AcrossBlocks) {
  const char* kScript = R"(
    import 'dart:typed_data';

    class Expect {
      static void equals(var a, var b) {}
      static void listEquals(var a, var b) {}
    }

    testViewAliasing1() {
      final f64 = new Float64List(1);
      final f32 = new Float32List.view(f64.buffer);
      f64[0] = 1.0; // Should not be forwarded.
      f32[1] = 2.0; // upper 32bits for 2.0f and 2.0 are the same
      return f64[0];
    }

    testViewAliasing2() {
      final f64 = new Float64List(2);
      final f64v = new Float64List.view(f64.buffer,
                                        Float64List.bytesPerElement);
      f64[1] = 1.0; // Should not be forwarded.
      f64v[0] = 2.0;
      return f64[1];
    }

    testViewAliasing3() {
      final u8 = new Uint8List(Float64List.bytesPerElement * 2);
      final f64 = new Float64List.view(u8.buffer, Float64List.bytesPerElement);
      f64[0] = 1.0; // Should not be forwarded.
      u8[15] = 0x40;
      u8[14] = 0x00;
      return f64[0];
    }

    main() {
      for (var i = 0; i < 20; i++) {
        Expect.equals(2.0, testViewAliasing1());
        Expect.equals(2.0, testViewAliasing2());
        Expect.equals(2.0, testViewAliasing3());
      }
    }
  )";

  const auto& root_library = Library::Handle(LoadTestScript(kScript));
  const auto& function =
      Function::Handle(GetFunction(root_library, "testViewAliasing1"));

  Invoke(root_library, "main");

  TestPipeline pipeline(function, CompilerPass::kJIT);
  // Recent changes actually compile the function into a single basic
  // block, so we need to test right after the load optimizer has been run.
  // Have checked that this test still fails appropriately using the load
  // optimizer prior to the fix (commit 2a237327).
  FlowGraph* flow_graph = pipeline.RunPasses({
      CompilerPass::kComputeSSA,
      CompilerPass::kApplyICData,
      CompilerPass::kTryOptimizePatterns,
      CompilerPass::kSetOuterInliningId,
      CompilerPass::kTypePropagation,
      CompilerPass::kApplyClassIds,
      CompilerPass::kInlining,
      CompilerPass::kTypePropagation,
      CompilerPass::kApplyClassIds,
      CompilerPass::kTypePropagation,
      CompilerPass::kApplyICData,
      CompilerPass::kCanonicalize,
      CompilerPass::kBranchSimplify,
      CompilerPass::kIfConvert,
      CompilerPass::kCanonicalize,
      CompilerPass::kConstantPropagation,
      CompilerPass::kOptimisticallySpecializeSmiPhis,
      CompilerPass::kTypePropagation,
      CompilerPass::kWidenSmiToInt32,
      CompilerPass::kSelectRepresentations,
      CompilerPass::kCSE,
  });

  auto entry = flow_graph->graph_entry()->normal_entry();
  EXPECT(entry != nullptr);

  StaticCallInstr* list_factory = nullptr;
  UnboxedConstantInstr* double_one = nullptr;
  StoreIndexedInstr* first_store = nullptr;
  StoreIndexedInstr* second_store = nullptr;
  LoadIndexedInstr* final_load = nullptr;
  BoxInstr* boxed_result = nullptr;

  ILMatcher cursor(flow_graph, entry);
  RELEASE_ASSERT(cursor.TryMatch(
      {
          {kMatchAndMoveStaticCall, &list_factory},
          kMatchAndMoveBranchTrue,
          kMatchAndMoveBranchTrue,
          kMatchAndMoveBranchFalse,
          kMatchAndMoveBranchFalse,
          {kMatchAndMoveUnboxedConstant, &double_one},
          {kMatchAndMoveStoreIndexed, &first_store},
          kMatchAndMoveBranchFalse,
          {kMatchAndMoveStoreIndexed, &second_store},
          {kMatchAndMoveLoadIndexed, &final_load},
          {kMatchAndMoveBox, &boxed_result},
          kMatchReturn,
      },
      /*insert_before=*/kMoveGlob));

  EXPECT(first_store->array()->definition() == list_factory);
  EXPECT(second_store->array()->definition() == list_factory);
  EXPECT(boxed_result->value()->definition() != double_one);
  EXPECT(boxed_result->value()->definition() == final_load);
}

static void CountLoadsStores(FlowGraph* flow_graph,
                             intptr_t* loads,
                             intptr_t* stores) {
  for (BlockIterator block_it = flow_graph->reverse_postorder_iterator();
       !block_it.Done(); block_it.Advance()) {
    for (ForwardInstructionIterator it(block_it.Current()); !it.Done();
         it.Advance()) {
      if (it.Current()->IsLoadField()) {
        (*loads)++;
      } else if (it.Current()->IsStoreInstanceField()) {
        (*stores)++;
      }
    }
  }
}

ISOLATE_UNIT_TEST_CASE(LoadOptimizer_RedundantStoresAndLoads) {
  const char* kScript = R"(
    class Bar {
      Bar() { a = null; }
      Object a;
    }

    Bar foo() {
      Bar bar = new Bar();
      bar.a = null;
      bar.a = bar;
      bar.a = bar.a;
      return bar.a;
    }

    main() {
      foo();
    }
  )";

  const auto& root_library = Library::Handle(LoadTestScript(kScript));
  Invoke(root_library, "main");
  const auto& function = Function::Handle(GetFunction(root_library, "foo"));
  TestPipeline pipeline(function, CompilerPass::kJIT);
  FlowGraph* flow_graph = pipeline.RunPasses({
      CompilerPass::kComputeSSA,
      CompilerPass::kTypePropagation,
      CompilerPass::kApplyICData,
      CompilerPass::kInlining,
      CompilerPass::kTypePropagation,
      CompilerPass::kSelectRepresentations,
      CompilerPass::kCanonicalize,
      CompilerPass::kConstantPropagation,
  });

  ASSERT(flow_graph != nullptr);

  // Before CSE, we have 2 loads and 4 stores.
  intptr_t bef_loads = 0;
  intptr_t bef_stores = 0;
  CountLoadsStores(flow_graph, &bef_loads, &bef_stores);
  EXPECT_EQ(2, bef_loads);
  EXPECT_EQ(4, bef_stores);

  DominatorBasedCSE::Optimize(flow_graph);

  // After CSE, no load and only one store remains.
  intptr_t aft_loads = 0;
  intptr_t aft_stores = 0;
  CountLoadsStores(flow_graph, &aft_loads, &aft_stores);
  EXPECT_EQ(0, aft_loads);
  EXPECT_EQ(1, aft_stores);
}

}  // namespace dart