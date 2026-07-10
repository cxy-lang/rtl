# Array Port Support Design Document

## Overview

This document describes the design and implementation of array support for `@input`, `@output`, `@inout`, and `@signal` attributes in the RTL plugin. This feature enables hardware components to have multiple ports of the same type accessed via array indexing.

## Motivation

### Problem Statement

Currently, components can only declare single ports:
```cxy
@rtl::component
class BufferAllocator[NumPorts] {
    @input allocReq: BitVector[NumPorts]  // ✅ Works - single port
    @input bufferId0: BufferId             // ❌ Not scalable
    @input bufferId1: BufferId
    @input bufferId2: BufferId
    // ... need NumPorts separate declarations
}
```

For scalable designs, we need:
```cxy
@rtl::component
class BufferDeallocator[NumPorts] {
    @input deallocReq: BitVector[NumPorts]
    @input deallocBufferId: [BufferId, #{NumPorts.value}]  // ✅ Desired: array of ports
}
```

### Use Cases

1. **Multi-port packet processors**: Each port has its own buffer ID, address, data
2. **Arbiters**: Multiple request/grant pairs
3. **Memory controllers**: Separate read/write channels per bank
4. **Bus interfaces**: Multiple master/slave connections

## Requirements

### Functional Requirements

**FR1**: Support array syntax for port declarations
- `@input x: [T, N]` where N is compile-time constant
- `@output y: [T, N]`
- `@inout z: [T, N]`
- `@signal s: [T, N]`

**FR2**: Array indexing must work with both static and dynamic indices
- Static: `x.[0]`, `x.[1]` (compile-time known)
- Dynamic: `x.[idx]` where `idx` is runtime variable

**FR3**: Constructor must accept tuple of signals
- Component instantiation: `MyComp(..., (sig0, sig1, sig2, ...), ...)`
- Type safety: tuple size must match array size

**FR4**: Internal storage as array for natural indexing
- Field type: `[InPort[T], N]` or `[OutPort[T], N]`
- Not tuple - preserves array semantics

**FR5**: @method sensitivity must handle array ports
- `@method(x)` where x is array → wait on any element change
- Use EventGroup internally to wait on all signals

### Non-Functional Requirements

**NFR1**: Compile-time validation
- Array size must be compile-time constant (`#const` or `#{expr}`)
- All array elements must be same type

**NFR2**: Clear error messages
- "Array ports not yet supported" → actionable guidance
- Type mismatch in tuple → show expected vs actual

**NFR3**: No runtime overhead
- Array iteration unrolled at compile time where possible
- No dynamic allocation

## Design

### Type Transformation Rules

#### Ports (Input/Output/InOut)

**Declaration:**
```cxy
@input x: [BufferId, 4]
```

**Field Type (internal storage):**
```cxy
x: [InPort[BufferId], 4]
```
- Array of ports enables indexing: `x.[0]`, `x.[idx]`
- Each element is a separate port instance

**Constructor Parameter:**
```cxy
func `init`(..., x: (Signal[BufferId], Signal[BufferId], Signal[BufferId], Signal[BufferId]), ...)
```
- Tuple type for parameter (not array)
- Caller passes tuple of individual signals: `(sig0, sig1, sig2, sig3)`
- Size must match array declaration

**Initialization:**
```cxy
// Generated in constructor body
// Plugin generates compile-time for loop
#for (const i: 0..N) {
    this.x.[#{i}] = InPort[BufferId](x.#{i})
}
```
- `#for` loop unrolls at user compile time when N is known
- Compile-time indexing on both array and tuple: `.[#{i}]` and `.#{i}`
- Each iteration connects one tuple element to one array port

#### Signals (Internal)

**Declaration:**
```cxy
@signal s: [Bit, 4] = Bit('0')
```
- User provides single default value
- Plugin duplicates for all array elements

**Field Type:**
```cxy
s: [Signal[Bit], 4]
```
- Array of signals
- No constructor parameter (internal only)

**Initialization:**
```cxy
// In generated init() or constructor
// Plugin generates compile-time for loop (unrolls at user compile time)
#for (const i: 0..N) {
    this.s.[#{i}] = Signal[Bit](Bit('0'))  // cloned from field default
}
```
- Plugin uses `deepCloneAstNode()` to replicate the default value
- `#for` loop unrolls when user instantiates component with concrete N
- Compile-time indexing: `.[#{i}]` uses constant index

#### BitVector Arrays

**Special case: BitVector arrays for BitSignal/BitPort:**

**Declaration:**
```cxy
@input x: [BitVector[8], 4]
```

**Field Type:**
```cxy
x: [InBitPort[8], 4]
```

**Constructor Parameter:**
```cxy
x: (BitSignal[8], BitSignal[8], BitSignal[8], BitSignal[8])
```

### AST Transformation Strategy

#### Detection Phase

Check if field type is array:
```c
bool isArrayType(const AstNode *typeNode)
{
    return typeNode && nodeIs(typeNode, ArrayType);
}
```

Array node structure:
```c
typeNode->arrayType.elementType  // Inner type T
typeNode->arrayType.dim          // Dimension expression (must be constant)
```

#### Extraction Phase

Extract array dimensions and element type:
```c
typedef struct {
    AstNode *elementType;  // T from [T, N]
    AstNode *dimExpr;      // N from [T, N] (must eval to constant)
    u64 dimValue;          // Evaluated dimension
} ArrayTypeInfo;

bool extractArrayInfo(const AstNode *arrayType, ArrayTypeInfo *info)
{
    info->elementType = arrayType->arrayType.elementType;
    info->dimExpr = arrayType->arrayType.dim;
    
    // Evaluate dimension expression at compile time
    // Must be constant (#const or #{expr})
    if (!evaluateConstExpr(info->dimExpr, &info->dimValue)) {
        return false;  // Error: dimension must be compile-time constant
    }
    
    if (info->dimValue == 0 || info->dimValue > MAX_ARRAY_SIZE) {
        return false;  // Error: invalid array size
    }
    
    return true;
}
```

#### Field Type Generation

Create array of port types:
```c
AstNode *makePortArrayType(MemPool *pool,
                           const FileLoc *loc,
                           cstring portTypeName,  // "InPort", "OutPort", etc.
                           AstNode *elementType,  // T
                           AstNode *dimExpr)      // N
{
    // Step 1: Create InPort[T]
    AstNode *wrappedType = makePortTypeWrapper(pool, loc, portTypeName, elementType);
    
    // Step 2: Create [InPort[T], N]
    AstNode *arrayType = makeArrayTypeAstNode(pool,
                                               loc,
                                               flgNone,
                                               wrappedType,  // element = InPort[T]
                                               dimExpr,      // dimension = N
                                               NULL);
    return arrayType;
}
```

#### Constructor Parameter Generation with `tupleof!()` Macro

**Problem:** Plugin runs during RTL library compilation, before users instantiate components. Array dimension N (e.g., `#{NumPorts.value}`) is a symbolic expression at plugin time and cannot be evaluated.

**Solution:** Use `tupleof!(T, N)` builtin macro to generate tuple type dynamically.

```c
// Generate macro call: tupleof!(Signal[T], N)
AstNode *makeTupleOfMacroCall(MemPool *pool,
                              const FileLoc *loc,
                              AstNode *elementType,  // Signal[T] or BitSignal[W]
                              AstNode *dimExpr)      // N expression (not evaluated)
{
    // Create macro identifier: tupleof
    AstNode *macroIdent = makeIdentifier(pool, loc, S_tupleof, 0, NULL, NULL);
    
    // Build argument list: (elementType, dimExpr)
    AstNode *args = elementType;
    elementType->next = dimExpr;
    
    // Create macro call: tupleof!(elementType, dimExpr)
    return makeMacroCallAstNode(pool, loc, flgNone, macroIdent, args, NULL);
}
```

**Generated Constructor Signature:**

For array port `@input x: [BufferId, #{NumPorts.value}]`:
```cxy
func `init`(..., x: tupleof!(Signal[BufferId], #{NumPorts.value}), ...)
```

At user compile time when `NumPorts = 4`:
- Macro expands to: `(Signal[BufferId], Signal[BufferId], Signal[BufferId], Signal[BufferId])`
- User passes tuple: `(sig0, sig1, sig2, sig3)`
- Type checking succeeds

**Benefits:**
- Plugin doesn't evaluate N (defers to user compile time)
- Works with generic parameters like `#{NumPorts.value}`
- Type-safe: compiler validates tuple size matches
- Clean syntax for users

#### Constructor Body Generation

Generate initialization code using compile-time for loops:
```c
AstNode *makeArrayPortInitCode(MemPool *pool,
                                const FileLoc *loc,
                                cstring fieldName,      // "deallocBufferId"
                                cstring paramName,      // constructor param name
                                cstring portTypeName,   // "InPort"
                                AstNode *elementType,   // BufferId
                                AstNode *dimExpr,       // N expression (may be generic)
                                AstNode *wrapperArgs)   // For BitPort[N], the N arg
{
    // Generate compile-time for loop:
    // #for (const i: 0..N) {
    //     this.fieldName.[#{i}] = InPort[T](paramName.#{i})
    // }
    
    // Create loop variable declaration: const i
    AstNode *loopVar = makeVarDecl(pool, loc, "i", makeAutoType(pool, loc), NULL);
    loopVar->flags |= flgConst;
    
    // Create range: 0..N
    AstNode *rangeExpr = makeRangeExpr(pool, loc,
                                        makeIntegerLiteral(pool, loc, 0),
                                        dimExpr);  // Use original dimension expression
    
    // Create loop body:
    // this.fieldName.[#{i}] = InPort[T](paramName.#{i})
    
    // Left side: this.fieldName.[#{i}]
    AstNode *comptimeIndex = makeComptimeExpr(pool, loc, makeIdentifier(pool, loc, "i"));
    AstNode *lhs = makeArrayIndexExpr(pool, loc,
                                       makeFieldAccessExpr(pool, loc, "this", fieldName),
                                       comptimeIndex);
    
    // Right side: paramName.#{i} (comptime tuple access)
    AstNode *tupleIndex = makeComptimeExpr(pool, loc, makeIdentifier(pool, loc, "i"));
    AstNode *tupleAccess = makeTupleMemberAccess(pool, loc,
                                                  makeIdentifier(pool, loc, paramName),
                                                  tupleIndex);
    
    // Port constructor call: InPort[T](paramName.#{i})
    AstNode *portConstructor;
    if (wrapperArgs) {
        // BitPort case: InBitPort[N](param.#{i})
        portConstructor = makeBitPortConstructorCall(pool, loc, portTypeName,
                                                      wrapperArgs, tupleAccess);
    } else {
        // Regular case: InPort[T](param.#{i})
        portConstructor = makePortConstructorCall(pool, loc, portTypeName,
                                                   elementType, tupleAccess);
    }
    
    // Assignment statement
    AstNode *assignStmt = makeAssignmentStmt(pool, loc, lhs, portConstructor);
    
    // Create comptime for loop
    AstNode *comptimeFor = makeComptimeForStmt(pool, loc,
                                                loopVar,      // const i
                                                rangeExpr,    // 0..N
                                                assignStmt);  // body
    
    return comptimeFor;
}
```

**Key points:**
- Plugin doesn't evaluate N - passes `dimExpr` directly to range
- Uses `#for` comptime loop construct
- Compile-time indexing with `#{i}` for both array and tuple access
- Loop unrolls at user compile time when generic parameters are concrete

### Sensitivity Handling (@method)

When @method depends on array port, must wait on all elements:

```c
AstNode *generateArrayPortWaitCode(MemPool *pool,
                                    const FileLoc *loc,
                                    cstring fieldName,
                                    AstNode *dimExpr)    // Use dimension expression, not concrete size
{
    // Generate EventGroup wait using compile-time for:
    // var eg = EventGroup()
    // #for (const i: 0..N) {
    //     eg.add(this.fieldName.[#{i}].valueChange())
    // }
    // eg.wait()
    
    // Create EventGroup
    AstNode *egDecl = makeVarDecl(pool, loc, "eg", makeEventGroupConstructor(pool, loc));
    
    // Create comptime for loop body:
    // eg.add(this.fieldName.[#{i}].valueChange())
    
    AstNode *loopVar = makeVarDecl(pool, loc, "i", makeAutoType(pool, loc), NULL);
    loopVar->flags |= flgConst;
    
    AstNode *rangeExpr = makeRangeExpr(pool, loc,
                                        makeIntegerLiteral(pool, loc, 0),
                                        dimExpr);  // Use original dimension expression
    
    // this.fieldName.[#{i}]
    AstNode *comptimeIndex = makeComptimeExpr(pool, loc, makeIdentifier(pool, loc, "i"));
    AstNode *portAccess = makeArrayIndexExpr(pool, loc,
                                              makeFieldAccessExpr(pool, loc, "this", fieldName),
                                              comptimeIndex);
    
    // portAccess.valueChange()
    AstNode *valueChange = makeMethodCall(pool, loc, portAccess, "valueChange");
    
    // eg.add(...)
    AstNode *addCall = makeMethodCall(pool, loc,
                                       makeIdentifier(pool, loc, "eg"),
                                       "add",
                                       valueChange);
    
    // Create comptime for loop
    AstNode *comptimeFor = makeComptimeForStmt(pool, loc, loopVar, rangeExpr, addCall);
    
    // eg.wait()
    AstNode *waitCall = makeMethodCall(pool, loc, makeIdentifier(pool, loc, "eg"), "wait");
    
    // Chain: var eg = ...; #for(...) { eg.add(...) }; eg.wait()
    egDecl->next = comptimeFor;
    comptimeFor->next = waitCall;
    
    return egDecl;
}
```

**Key points:**
- Plugin generates `#for` loop with dimension expression (not concrete size)
- Loop unrolls at user compile time
- Each iteration adds one port's event to EventGroup
- EventGroup waits on all ports simultaneously

## Implementation Plan

### Phase 1: Detection and Validation

**File**: `src/plugin/discovery.c`

**Changes**:
1. Modify `analyzePortField()` to detect array types
2. Store array info in `RtlPortInfo`:
   ```c
   typedef struct RtlPortInfo {
       // ... existing fields ...
       bool isArray;
       AstNode *arrayDimExpr;     // Store expression, not evaluated size
       AstNode *arrayElementType; // T from [T, N]
   } RtlPortInfo;
   ```
3. Add validation:
   - Array dimension must be expression (defer evaluation to user compile time)
   - Element type must be valid port type
   - No nested arrays (e.g., `[[T, N], M]` not supported initially)

**Note**: Plugin does NOT evaluate dimension expression. It remains symbolic (e.g., `#{NumPorts.value}`).

**Error Messages**:
- "Array element type {type} is not valid for ports"
- "Nested array ports are not supported"

### Phase 2: Type Transformation

**File**: `src/plugin/transform.c`

**Changes**:
1. Modify `transformPortField()`:
   ```c
   if (port->isArray) {
       // Generate [InPort[T], N] field type
       field->structField.type = makePortArrayType(...);
       
       // Store tuple type for constructor parameter
       port->paramType = makeTupleTypeForArrayPort(...);
   } else {
       // Existing single-port logic
       ...
   }
   ```

2. Handle BitVector arrays:
   ```c
   if (port->isArray && isBitVectorType(port->elementType)) {
       // [BitVector[W], N] → [InBitPort[W], N]
       // Constructor param: (BitSignal[W], ..., BitSignal[W])
       ...
   }
   ```

3. Modify `transformSignalField()` similarly for @signal arrays

### Phase 3: Constructor Generation

**File**: `src/plugin/codegen-wrapper.c`

**Changes**:
1. Modify `generateConstructor()` to handle array ports:
   ```c
   for each port {
       if (port->isArray) {
           // Add tuple parameter
           addConstructorParam(port->name, port->tupleParamType);
           
           // Generate compile-time for loop for initialization
           AstNode *initCode = makeArrayPortInitCode(pool, loc,
                                                      port->name,
                                                      port->paramName,
                                                      port->portTypeName,
                                                      port->elementType,
                                                      port->arrayDimExpr,  // Pass expression, not size
                                                      port->wrapperArgs);
           appendToConstructorBody(initCode);
       } else {
           // Existing single-port logic
           ...
       }
   }
   ```

2. Generate `#for` loop that unrolls at user compile time

### Phase 4: Method Sensitivity

**File**: `src/plugin/codegen-wrapper.c`

**Changes**:
1. Modify `generateProcessMethod()`:
   ```c
   for each trigger in @method(...) {
       if (trigger is array port) {
           // Generate EventGroup wait code with #for loop
           AstNode *waitCode = generateArrayPortWaitCode(pool, loc,
                                                          port->name,
                                                          port->arrayDimExpr);  // Pass expression
           prependToMethodBody(waitCode);
       } else {
           // Existing single-signal wait
           ...
       }
   }
   ```

2. Generate `#for` loop that unrolls at user compile time

### Phase 5: Testing

**Test Cases**:

1. **Basic array port**:
   ```cxy
   @rtl::component
   class ArrayTest {
       @input x: [i32, 4]
       @output y: [i32, 4]
   }
   ```

2. **BitVector array**:
   ```cxy
   @rtl::component
   class BitArrayTest {
       @input data: [BitVector[8], 4]
   }
   ```

3. **Array indexing**:
   ```cxy
   @method(clk: "posedge")
   func process() {
       for i in 0..4 {
           y << x.[i]
       }
   }
   ```

4. **Dynamic indexing**:
   ```cxy
   var idx = computeIndex()
   y << x.[idx]
   ```

5. **Array signal sensitivity**:
   ```cxy
   @method(x)
   func onXChange() {
       // Waits on any x element change
   }
   ```

6. **Error cases**:
   - Non-constant array size
   - Nested arrays
   - Invalid element type

## Migration Path

### Backward Compatibility

All existing code continues to work - no breaking changes. Array syntax is additive.

### Deprecation

None. This is a new feature, not a replacement.

## Open Questions

1. **Nested arrays**: Should `[[ T, N], M]` be supported?
   - **Decision**: No, not initially. Wait for use case.

2. **Variable-length arrays**: Dynamic sizing at runtime?
   - **Decision**: No. Hardware dimensions are fixed at synthesis.

3. **Tuple unpacking syntax**: Should constructor accept both tuple and unpacked args?
   ```cxy
   MyComp(..., sig0, sig1, sig2, sig3)  // Unpacked
   MyComp(..., (sig0, sig1, sig2, sig3)) // Tuple
   ```
   - **Decision**: Require tuple for clarity. Arrays → tuples is explicit.

4. **Helper for EventGroup**: Add to RTL library?
   ```cxy
   func waitAny[...Signals](signals: Signals): void
   ```
   - **Decision**: Yes, add helper to rtl/src/event-group.cxy

## Success Criteria

1. ✅ Can declare array ports with compile-time constant size
2. ✅ Field type is array: `[InPort[T], N]`
3. ✅ Constructor parameter is tuple: `(Signal[T], ..., Signal[T])`
4. ✅ Indexing works: `x.[0]`, `x.[idx]`
5. ✅ @method sensitivity handles array ports (EventGroup)
6. ✅ BitVector arrays work: `[BitVector[W], N]` → `[InBitPort[W], N]`
7. ✅ @signal arrays work: `@signal s: [Bit, N]`
8. ✅ All tests pass
9. ✅ Clear error messages for invalid usage
10. ✅ No performance regression on existing single-port code

## Appendix: Example Generated Code

### Input Code
```cxy
@rtl::component
class Dealloc[NumPorts] {
    @input clk: Clock
    @input deallocReq: BitVector[NumPorts]
    @input deallocBufferId: [BufferId, #{NumPorts.value}]
    
    @output deallocAck: BitVector[NumPorts]
    
    @method(clk: "posedge")
    func process() {
        for i in 0..#{NumPorts.value} {
            if deallocReq.[i] == Bit('1') {
                handleDealloc(deallocBufferId.[i])
            }
        }
    }
}
```

### Generated Code (Conceptual)
```cxy
class Dealloc[NumPorts] {
    clk: &Clock
    deallocReq: InBitPort[NumPorts]
    deallocBufferId: [InPort[BufferId], #{NumPorts.value}]  // Array field
    deallocAck: OutBitPort[NumPorts]
    
    func `init`(clk: &Clock,
                deallocReq: &BitSignal[NumPorts],
                deallocBufferId: (Signal[BufferId], Signal[BufferId], Signal[BufferId], Signal[BufferId]),  // Tuple param
                deallocAck: &BitSignal[NumPorts]) {
        this.clk = clk
        this.deallocReq = InBitPort[NumPorts](deallocReq)
        
        // Unrolled initialization
        this.deallocBufferId.[0] = InPort[BufferId](deallocBufferId.0)
        this.deallocBufferId.[1] = InPort[BufferId](deallocBufferId.1)
        this.deallocBufferId.[2] = InPort[BufferId](deallocBufferId.2)
        this.deallocBufferId.[3] = InPort[BufferId](deallocBufferId.3)
        
        this.deallocAck = OutBitPort[NumPorts](deallocAck)
    }
    
    func process() {
        for i in 0..#{NumPorts.value} {
            if deallocReq.[i] == Bit('1') {
                handleDealloc(deallocBufferId.[i])
            }
        }
    }
}
```

### Usage
```cxy
var clk = Clock("clk", 10.ns)
var reqSig = BitSignal[4](0)
var id0 = Signal[BufferId](0.u8)
var id1 = Signal[BufferId](0.u8)
var id2 = Signal[BufferId](0.u8)
var id3 = Signal[BufferId](0.u8)
var ackSig = BitSignal[4](0)

var dealloc = Dealloc[4](&clk, reqSig, (id0, id1, id2, id3), ackSig)
//                                       ^^^^^^^^^^^^^^^^^^^^ Tuple
```

## References

- RTL Design Document: `rtl/docs/design.md`
- Plugin Implementation: `rtl/src/plugin/`
- Cxy AST Nodes: `cxy/include/cxy/lang/node.h`
