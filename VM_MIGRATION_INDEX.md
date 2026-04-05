# Fleaux VM Migration Analysis - Complete Package

## Overview

This directory contains a comprehensive analysis of converting Fleaux from a C++ transpiler to a VM-based interpreter for embedding in C++.

**Status:** Analysis complete. Ready for Phase 1 (Design & Prototype).

---

## Documents

### 📋 Start Here
- **[README_VM_MIGRATION.md](README_VM_MIGRATION.md)** - Quick start guide and document index

### 📊 Main Analysis
- **[VM_MIGRATION_ANALYSIS.md](VM_MIGRATION_ANALYSIS.md)** - Complete analysis with all details
  - What can be reused
  - What needs to be built (6 components)
  - Effort breakdown by component
  - Implementation phases (7 stages)
  - Key challenges and gotchas
  - Risk assessment and mitigation
  - Performance trade-offs
  
  **→ Read this for comprehensive understanding**

### 🏗️ Technical Design
- **[BYTECODE_FORMAT_SPEC.md](BYTECODE_FORMAT_SPEC.md)** - Concrete bytecode format specification
  - 25+ opcodes with semantics
  - File format structure (header, pools, metadata)
  - Instruction encoding details
  - Real bytecode examples
  - Memory layout diagrams
  - Serialization/deserialization pseudocode
  
  **→ Read this for technical implementation details**

### 🎯 Architecture Decisions
- **[VM_DECISIONS.md](VM_DECISIONS.md)** - 10 critical architectural decisions
  - Stack-based vs. register-based execution
  - Value representation (keep DataTree or custom)
  - Module serialization format
  - Type system metadata
  - Function representation (bytecode closures)
  - Performance optimization approach
  - Control-flow primitives
  - Error diagnostics
  - Embedding API design
  - JIT compilation strategy
  
  **→ Read this for design review and team alignment**

### 📈 Visual Summary
- **[VM_MIGRATION_SUMMARY.txt](VM_MIGRATION_SUMMARY.txt)** - ASCII art summary for quick reference
  - Effort breakdown
  - Phase timeline
  - Key challenges
  - Architecture decisions table
  - Success criteria
  
  **→ Read this for quick overview**

---

## Quick Facts

| Aspect | Value |
|--------|-------|
| **Total Effort** | 20-27 weeks (5-7 developer-months) |
| **Reusable Code** | Parser, AST, IR, type system, all 150+ builtins |
| **New Components** | Instruction set, compiler, interpreter, adapters, metadata |
| **Execution Model** | Stack-based VM (recommended) |
| **Performance** | 5-10× slower than native C++ (acceptable with JIT) |
| **Key Challenge** | Control-flow primitives need bytecode closures |
| **Riskiest Phase** | Phase 2 (VM interpreter) - needs careful integration |
| **Phase 1 Duration** | 3 weeks (design + prototype) |

---

## Component Reusability

### ✅ Fully Reusable (No Changes)
- `fleaux_parser.py` - Parsing unchanged
- `fleaux_lowering.py` - AST → IR conversion unchanged
- `fleaux_ast.py` - IR structure unchanged
- `fleaux_runtime.hpp` - All 150+ C++ builtins work as-is
- DataTree value model - Same representation
- Type system logic - Validation rules unchanged
- Test suite - 1648 tests remain valid

### ❌ Needs Complete Rebuild
- C++ code generation → Bytecode compilation
- C++ compiler integration → VM interpreter
- Transpiler entry point (run_fleaux.py)

---

## Implementation Roadmap

```
PHASE 1: Design & Prototype (Weeks 1-3)
├─ Week 1: Bytecode format design
├─ Week 2: Compiler prototype (simple expressions)
└─ Week 3: Validation on samples

PHASE 2: Core VM (Weeks 4-9)
├─ Weeks 4-5: Complete bytecode compiler
└─ Weeks 6-9: VM interpreter

PHASE 3: Features (Weeks 10-16)
├─ Weeks 10-11: Control-flow (Loop, Branch, Select)
├─ Weeks 12-13: Module system & type checking
└─ Weeks 14-16: Python transpiler refactor

PHASE 4: Polish (Weeks 17-27)
├─ Weeks 17-19: Testing & debugging
├─ Weeks 20-22: Performance optimization
└─ Weeks 23-24: Documentation & embedding examples
```

---

## Recommended Architecture

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Execution | Stack-based | Simpler, matches expressions |
| Values | Keep DataTree | Reuse 150+ builtins |
| Format | Plain binary | Compact, no dependencies |
| Metadata | Minimal (Phase 1) | Fast to implement, enhance later |
| Functions | Bytecode closures | Pure VM approach, clean semantics |
| Optimization | Minimal (Phase 1) | Profile first, optimize later |
| Control Flow | Special opcodes | DO_LOOP, DO_BRANCH, etc. |
| Diagnostics | Source locations | Good error messages |
| Embedding API | Functional (Phase 1) | Simple `execute()` first |
| JIT | Phase 2+ (optional) | Add if performance needs it |

---

## Key Challenges

1. **Control-Flow Primitives** (Medium Risk, High Impact)
   - Loop/Branch/Select currently use C++ lambdas
   - VM needs bytecode closures
   - Requires special handling
   - Mitigation: Prototype in week 2-3

2. **First-Class Functions** (Medium Risk, High Impact)
   - Functions must be values
   - Closures with capture
   - Serializable
   - Mitigation: Design early, test thoroughly

3. **Performance** (Medium Risk, Medium Impact)
   - VM is 5-10× slower than native
   - Acceptable for embedding, but measure baseline
   - Mitigation: JIT compilation as Phase 2

4. **Type System** (Low Risk, Low Impact)
   - Need runtime type metadata
   - Balance size vs. diagnostics
   - Mitigation: Start minimal, enhance iteratively

---

## Success Criteria

### Phase 1 ✓
- [ ] Bytecode format documented
- [ ] Instruction set finalized
- [ ] Simple program compiles correctly
- [ ] Bytecode size reasonable

### Phase 2 ✓
- [ ] VM runs simple programs
- [ ] Basic expressions work
- [ ] Builtins dispatch correctly

### Phase 3 ✓
- [ ] Control-flow works
- [ ] Module system works
- [ ] Full pipeline operational

### Phase 4 ✓
- [ ] 1648 tests pass
- [ ] Performance acceptable
- [ ] Embedding API works
- [ ] Documentation complete

---

## Performance Baseline (TO DO)

Before starting Phase 1, measure:

```bash
# Time the transpiler on a realistic program
time ./fleaux realistic_program.fleaux

# Record:
# - Parse time
# - Transpile time
# - C++ compile time
# - Execution time

# This baseline will help measure VM success
```

---

## Team Questions to Discuss

- [ ] Is embedding the PRIMARY goal?
- [ ] What's acceptable performance overhead (2×? 5×? 10×)?
- [ ] How many developers available?
- [ ] Any hard deadline?
- [ ] Need interactive debugging or just error messages?
- [ ] Support hot-reloading of modules?
- [ ] Constraints on bytecode file size?

---

## Next Actions

### Immediate (This Week)
1. Read [VM_MIGRATION_ANALYSIS.md](VM_MIGRATION_ANALYSIS.md)
2. Review [VM_DECISIONS.md](VM_DECISIONS.md)
3. Discuss with team, identify any concerns

### Week 1
1. Finalize bytecode format (refine [BYTECODE_FORMAT_SPEC.md](BYTECODE_FORMAT_SPEC.md))
2. Resolve all 10 architecture decisions
3. Set performance baseline on current transpiler
4. Schedule Phase 1 kickoff

### Weeks 1-3 (Phase 1)
1. Build bytecode format reader/writer
2. Implement IR → bytecode compiler (simple expressions)
3. Prototype VM interpreter
4. Measure bytecode quality
5. Decide: proceed with full implementation?

---

## Reference Documentation

### Bytecode Format
- Instruction opcodes: [BYTECODE_FORMAT_SPEC.md](BYTECODE_FORMAT_SPEC.md#instruction-set-provisional)
- File format: [BYTECODE_FORMAT_SPEC.md](BYTECODE_FORMAT_SPEC.md#bytecode-file-format)
- Example bytecode: [BYTECODE_FORMAT_SPEC.md](BYTECODE_FORMAT_SPEC.md#example-bytecode-for-1-2---stdadd---stdprintln)

### Architecture
- Execution model: [VM_DECISIONS.md](VM_DECISIONS.md#decision-1-execution-model)
- Value representation: [VM_DECISIONS.md](VM_DECISIONS.md#decision-2-value-representation)
- All 10 decisions: [VM_DECISIONS.md](VM_DECISIONS.md)

### Analysis
- Component breakdown: [VM_MIGRATION_ANALYSIS.md](VM_MIGRATION_ANALYSIS.md#what-can-be-reused-no-changes)
- Effort by component: [VM_MIGRATION_ANALYSIS.md](VM_MIGRATION_ANALYSIS.md#effort-estimate-by-component)
- Implementation order: [VM_MIGRATION_ANALYSIS.md](VM_MIGRATION_ANALYSIS.md#implementation-order-critical-path)
- Challenges & risks: [VM_MIGRATION_ANALYSIS.md](VM_MIGRATION_ANALYSIS.md#key-challenges--gotchas)

---

## File Organization

```
fleauxlang/
├── README.md (original - Fleaux overview)
├── 
├── FLEAUX VM MIGRATION ANALYSIS (This Package)
├── ├── README_VM_MIGRATION.md (Start here - index & overview)
├── ├── VM_MIGRATION_ANALYSIS.md (Main document - comprehensive analysis)
├── ├── VM_MIGRATION_SUMMARY.txt (ASCII summary for quick reference)
├── ├── BYTECODE_FORMAT_SPEC.md (Technical design - opcodes & format)
├── ├── VM_DECISIONS.md (Architecture decisions & rationale)
├── └── VM_MIGRATION_INDEX.md (This file - document guide)
├──
├── CURRENT IMPLEMENTATION (to be refactored)
├── ├── fleaux_parser.py (✅ Keep as-is)
├── ├── fleaux_lowering.py (✅ Keep as-is)
├── ├── fleaux_ast.py (✅ Keep as-is)
├── ├── fleaux_cpp_transpiler.py (❌ Replace with bytecode compiler)
├── ├── run_fleaux.py (❌ Update for VM execution)
├── ├── Std.fleaux (✅ Keep as-is)
├── └── cpp/fleaux_runtime.hpp (✅ Keep builtins)
├──
├── TESTS
└── └── tests/test_cpp_backend.py (1648 tests - will rerun on VM)
```

---

## Contact / Questions

Refer to the specific documents:
- **Why VM?** → [VM_MIGRATION_ANALYSIS.md](VM_MIGRATION_ANALYSIS.md#executive-summary)
- **How to build it?** → [BYTECODE_FORMAT_SPEC.md](BYTECODE_FORMAT_SPEC.md)
- **Which design choices?** → [VM_DECISIONS.md](VM_DECISIONS.md)
- **Quick overview?** → [VM_MIGRATION_SUMMARY.txt](VM_MIGRATION_SUMMARY.txt)

---

**Last Updated:** April 4, 2026  
**Status:** ✅ Analysis Complete, Ready for Phase 1  
**Next Phase:** Design & Prototype (3 weeks)

