# RTL Library Design Document
## System-Level Hardware Modeling in Cxy

**Version:** 0.1.0  
**Date:** May 2026  
**Author:** Carter Mbotho

---

## Table of Contents

1. [Introduction & Theory](#1-introduction--theory)
2. [Architecture Overview](#2-architecture-overview)
3. [Core Library Design](#3-core-library-design)
4. [Plugin System Design](#4-plugin-system-design)
5. [Simulation Kernel](#5-simulation-kernel)
6. [Implementation Plan](#6-implementation-plan)

---

## 1. Introduction & Theory

### 1.1 What is System-Level Modeling?

System-level modeling is an approach to hardware design that operates at a higher level of abstraction than traditional Register Transfer Level (RTL). Instead of describing every flip-flop and logic gate, system-level models focus on the **behavior** and **communication patterns** of hardware components.

Think of it like the difference between assembly language and a high-level programming language. Just as C abstracts away register allocation and instruction selection, system-level modeling abstracts away gate-level implementation details. You describe **what** the hardware should do, not exactly **how** it should be implemented at the gate level.

The key characteristics that distinguish system-level modeling are:

**Concurrent Process Model:** Hardware is inherently parallel - multiple components operate simultaneously. System-level models embrace this with explicit concurrent processes that represent independent hardware units running in parallel. Each process models a distinct hardware module or activity.

**Event-Driven Execution:** Rather than simulating every clock cycle exhaustively, system-level simulators use event-driven execution. Processes sleep until something interesting happens (a clock edge, a signal change, etc.), then wake up, perform their work, and go back to sleep. This is vastly more efficient than cycle-accurate simulation for early design exploration.

**Communication via Signals:** Hardware components communicate through wires, buses, and other interconnects. System-level models abstract these as **signals** - named values that processes can read from and write to. Signals handle the complexity of timing, synchronization, and value propagation.

**Timing Relationships:** While abstracted, timing still matters. Models capture clock relationships, setup/hold constraints, and temporal ordering without requiring gate-level timing analysis.

### 1.2 SystemC: The Foundation We're Building On

SystemC is the IEEE standard for system-level modeling (IEEE 1666). It's implemented as a C++ library with a simulation kernel, making it widely adopted in industry. Understanding SystemC helps us appreciate what we're building in Cxy.

#### How SystemC Works

At its core, SystemC provides three things:

**1. Modules - The Building Blocks**

A SystemC module represents a hardware component. It's like a struct or class that encapsulates:
- **Ports** for inputs and outputs (e.g., `sc_in<bool> reset`)
- **Internal state** (registers, counters, memories)
- **Processes** that describe behavior (what the module does)

Modules compose hierarchically - a processor module might contain ALU, register file, and control modules.

**2. Processes - Concurrent Behavior**

Processes are functions that run concurrently, modeling parallel hardware execution. SystemC provides three process types:

- **SC_METHOD:** Runs to completion, triggered by events, cannot suspend mid-execution. Like combinational logic that evaluates whenever inputs change.
  
- **SC_THREAD:** Can suspend and resume, models sequential processes like state machines. Can wait() at arbitrary points.
  
- **SC_CTHREAD:** Specialized thread synchronized to a clock edge.

Each process has a **sensitivity list** - the events that wake it up. For example, a process might be sensitive to the rising edge of a clock or any change on an input signal.

**3. The Simulation Kernel - Time and Events**

SystemC's kernel manages simulation time and event scheduling. The key concept is the **delta cycle**:

Imagine time advances in discrete steps. At each time point (say, 10ns), multiple things might happen:
- A clock edge triggers a process
- That process updates a signal
- The signal change triggers another process
- That process updates another signal
- ... and so on

All of this happens at "10ns" but needs ordering. SystemC uses **delta cycles** - conceptual sub-steps at the same simulation time:

```
Time 10ns:
  Delta 0: Clock edge fires → Process A runs → Schedules signal X update
  Delta 1: Signal X update commits → Triggers Process B → Schedules signal Y update  
  Delta 2: Signal Y update commits → No more activity
  (Deltas continue until no new events)
  
Time advances to 15ns:
  Delta 0: ...
```

This provides deterministic ordering without race conditions. Signal updates are **deferred** - they don't take effect immediately, but are scheduled for the next delta cycle. This mirrors how real hardware has propagation delays.

#### Why SystemC Succeeded

SystemC became the industry standard because:

- **Familiarity:** It's C++, which hardware engineers already know
- **Performance:** Compiled C++ simulates fast
- **Integration:** Works with existing C/C++ testbenches and tools
- **Proven semantics:** The delta cycle model is well-understood and matches hardware behavior

But SystemC has limitations:

- **Verbose syntax:** Heavy use of macros (`SC_MODULE`, `SC_CTOR`, `SC_METHOD`)
- **Manual bookkeeping:** Must manually register processes, declare sensitivity lists
- **C++ complexity:** Template metaprogramming, pointer management, undefined behavior
- **Non-native concurrency:** Builds cooperative threading on top of C++

### 1.3 Why Cxy is Uniquely Suited for This

Cxy brings language-level features that make system-level modeling more natural and safer than SystemC's library approach.

#### Native Symmetric Coroutines

Hardware processes naturally map to coroutines. Each hardware module that does something over time becomes a coroutine. Consider a simple counter:

```
Forever:
  Wait for clock edge
  Increment counter value
  Write value to output
```

This **is** a coroutine - a loop that suspends at well-defined points and resumes when events occur.

Cxy provides **symmetric coroutines** built into the language. Unlike stackful fibers or async/await, symmetric coroutines give you explicit control:

- Use `async { }` to launch a coroutine
- Inside a coroutine, call `running()` to get your own handle  
- Suspend yourself with the handle's `suspend()` method
- Other code can `resume()` you when appropriate

For our RTL library, this means:
- Each `@method` becomes an `async` function naturally
- The `wait()` function can internally call `running()` to get the current coroutine
- The simulation kernel maintains lists of waiting coroutines per event
- When an event fires, the kernel calls `resume()` on all waiting coroutines

No fibers library, no callback hell, no manual stack management. It's native.

#### Compile-Time AST Transformation via Plugins

SystemC requires verbose boilerplate:

```cpp
SC_MODULE(Counter) {
    sc_in<sc_clock> clk;
    sc_out<int> count;
    
    void process();
    
    SC_CTOR(Counter) {
        SC_METHOD(process);
        sensitive << clk.pos();
    }
};
```

Compare to Cxy with our plugin:

```cxy
@rtl::component
struct Counter {
    @in clk: Clock
    @out count: u32
    
    @method(clk: "posedge")
    func process() { ... }
}
```

The plugin **sees the AST** - the parsed structure of your code - and can transform it at compile time:

- Sees `@in clk: Clock` → Transforms field type to `InPort[Clock]`
- Sees `@method(clk: "posedge")` → Generates an `async` wrapper function
- Sees all ports → Generates a constructor that binds them
- Validates everything → Catches errors before runtime

This is more powerful than C++ templates or macros because:
- Full access to type information and semantic analysis
- Can generate arbitrary code (methods, fields, types)
- Can validate domain-specific rules (e.g., "input ports can't be written")
- All happens at compile time - zero runtime overhead
- Generated code is debuggable - it's just Cxy code

#### Strong Type System

Cxy's type system prevents common hardware modeling mistakes:

- **InPort vs OutPort:** Type system enforces that inputs can't be written, outputs can't be read
- **Signal[T]:** Generic types ensure type safety across signal connections
- **References not pointers:** Can't have dangling pointers to signals
- **Pattern matching:** Clean state machine implementation without error-prone switches

### 1.4 Our Design Goals

Based on SystemC's proven approach and Cxy's unique capabilities, our design goals are:

**1. Familiar to SystemC Users**

We adopt SystemC's core concepts - modules, ports, signals, processes, delta cycles - so hardware engineers with SystemC experience feel at home. The semantics should be compatible where it matters (simulation behavior, timing).

**2. Leverage Cxy's Strengths**

Use coroutines for processes (not macros), use plugins for boilerplate (not manual registration), use the type system for safety (not runtime checks).

**3. Cleaner Than SystemC**

The user should write clean, declarative code. The plugin generates the glue code. Reading a component definition should be obvious - ports, state, behavior - without hunting through macro expansions.

**4. Correct by Construction**

Use compile-time validation to catch errors:
- Wrong port directions
- Type mismatches in connections
- Invalid trigger specifications
- Unbound ports

Better to fail at compile time with a clear message than crash at runtime.

**5. Efficient Simulation**

While we prioritize correctness and usability, simulation speed matters. Use efficient data structures (binary heaps for event queues), minimize overhead in the hot path (signal updates, event scheduling), and leverage Cxy's lightweight coroutines.

### 1.5 What We're Building

Our RTL library consists of three layers:

**Runtime Library:** Core types (Signal, Clock, Event, etc.) and the simulation kernel. This is traditional library code written in Cxy. It provides the primitives but doesn't know about user components.

**Plugin:** Compile-time AST transformation that generates the glue code. It sees `@rtl::component` and friends, validates them, transforms types, and injects generated code. Users never call the plugin directly - it runs automatically during compilation.

**User Code:** Clean component definitions using attributes like `@in`, `@out`, `@method`. This is what hardware engineers write. It's declarative and intention-revealing.

The genius is in the separation: the library provides powerful primitives, the plugin does the tedious bookkeeping, and the user writes expressive hardware descriptions.

---

## 2. Architecture Overview

### 2.1 The Three-Layer Architecture

Our design separates concerns into three distinct layers:

```
┌────────────────────────────────────────────────────┐
│                  USER CODE LAYER                   │
│                                                    │
│  What users write:                                 │
│  - @rtl::component structs                         │
│  - @in/@out ports                                  │
│  - @method processes                               │
│  - Clean, declarative syntax                       │
└─────────────────────┬──────────────────────────────┘
                      │
                      │ At Compile Time
                      ▼
┌────────────────────────────────────────────────────┐
│                   PLUGIN LAYER                     │
│                                                    │
│  AST Transformations:                              │
│  - Discovers @rtl::component structs               │
│  - Transforms @in/@out field types                 │
│  - Generates constructors with binding logic       │
│  - Generates async process wrappers                │
│  - Validates all attributes and types              │
└─────────────────────┬──────────────────────────────┘
                      │
                      │ Generates & Injects Code
                      ▼
┌────────────────────────────────────────────────────┐
│                  RUNTIME LIBRARY                   │
│                                                    │
│  Core Types:           Simulation Kernel:          │
│  - Signal[T], Bit      - Event queue               │
│  - Clock, Bus          - Delta cycle manager       │
│  - InPort/OutPort      - Coroutine scheduling      │
│  - Event, Time         - Time advancement          │
│  - SimContext          - wait() implementation     │
└────────────────────────────────────────────────────┘
```

**User Code Layer** is what hardware engineers interact with. They write component definitions using attributes to declare intent (`@in` means this is an input port). They never write binding code, never manually launch coroutines, never call `resume()` on coroutines. The code reads like a hardware specification.

**Plugin Layer** runs at compile time. It analyzes the AST, performs transformations, generates missing code (constructors, wrappers), and validates everything. If there's a problem (wrong type, invalid trigger), it reports a compile error with a clear message. The user never directly invokes the plugin.

**Runtime Library** provides the simulation infrastructure. It doesn't know about specific components - it just provides Signal, Event, SimContext, etc. The generated code calls into these primitives. For example, generated code might call `wait(event)`, and `wait()` is implemented in SimContext.

This separation means:
- Library code is reusable and simple
- Plugin handles domain-specific complexity
- User code is clean and maintainable

### 2.2 Component Lifecycle: From Code to Simulation

Let's trace a component through all three layers to understand how they interact.

**Step 1: User Writes Component**

```cxy
@rtl::component
struct Counter {
    @in clk: Clock
    @in reset: Bit
    @out count: u32
    
    _value: u32 = 0
    
    @method(clk: "posedge")
    func process() {
        if (reset.read() == 1) {
            _value = 0
        } else {
            _value += 1
        }
        count << _value
    }
}
```

The user's intent is clear: "This is a component with a clock input, reset input, count output. On each rising clock edge, if reset is high, zero the value; otherwise increment it. Write the value to the output."

**Step 2: Plugin Transforms at Compile Time**

The plugin sees the `@rtl::component` attribute and processes the struct:

First, it **discovers ports** by scanning fields:
- Found `@in clk: Clock` → This is an input port of type Clock
- Found `@in reset: Bit` → Input port of type Bit
- Found `@out count: u32` → Output port of type u32

Next, it **transforms field types**:
- `@in clk: Clock` becomes `clk: InPort[Clock]`
- `@in reset: Bit` becomes `reset: InPort[Bit]`
- `@out count: u32` becomes `count: OutPort[u32]`

Why? Because InPort and OutPort provide:
- Direction enforcement (can't write to input)
- Binding semantics (connecting to external signals)
- Uniform interface regardless of underlying signal type

Then it **generates a constructor**:
```cxy
func init(clk: &Clock, reset: &Bit, count: &Signal[u32]) {
    this.clk.bind(clk)
    this.reset.bind(reset)
    this.count.bind(count)
    async _process_wrapper()
}
```

The constructor takes references to external signals, binds the ports to them, and launches the process coroutine.

Finally, it **generates the process wrapper**:
```cxy
private async _process_wrapper() {
    while (true) {
        wait(this.clk.posedge())
        process()
    }
}
```

This wrapper loops forever, waiting for the clock's rising edge event, then calling the user's `process()` function.

**Step 3: User Instantiates in Testbench**

```cxy
var clk = Clock("clk", 10.ns)
var reset = Bit("reset", 0)
var count = Signal[u32]("count", 0)

var counter = Counter(&clk, &reset, &count)
```

When `Counter(...)` is called, the generated constructor runs:
- Binds ports to the provided signals
- Launches `async _process_wrapper()`

The `async _process_wrapper()` coroutine starts immediately:
- Enters the while loop
- Calls `wait(this.clk.posedge())`

Inside `wait()` (implemented in SimContext):
- Calls `running()` to get the current coroutine handle
- Adds this coroutine to the clock's posedge event wait list
- Calls `suspend()` on the coroutine

The coroutine is now suspended, waiting for the clock edge.

**Step 4: Simulation Runs**

The testbench calls `ctx.run(100.ns)`. The simulation kernel's event loop runs:

At time 10ns, the clock's posedge event fires:
- The kernel looks up which coroutines are waiting on this event
- Finds our Counter's `_process_wrapper` coroutine
- Calls `resume()` on it

The coroutine wakes up, returning from the `wait()` call:
- Calls `process()` (the user's function)
- `process()` reads reset, updates _value, writes count
- Returns to the wrapper's while loop
- Calls `wait()` again, suspending until the next edge

This cycle continues for the duration of simulation.

### 2.3 Signal Flow and Event Propagation

Understanding how data flows through signals and how events propagate is key to the design.

**The Signal Update Problem**

Consider two processes:

```
Process A:                    Process B:
wait(clk.posedge())          wait(data.value_change())
data << 42                   if (data.read() > 40)
                                 output << 1
```

If signal writes happened immediately, we'd have a race condition: Does Process B see the new value in the same delta cycle? What if they both update signals that the other reads?

SystemC's solution (which we adopt): **Deferred signal updates**.

When Process A writes `data << 42`, this doesn't immediately change data's value. Instead:
1. The write is **scheduled** for the next delta cycle
2. A value_change event is **scheduled** for next delta
3. Process A continues (maybe reads data again - still sees old value)

At the end of the delta cycle, the simulation kernel **commits** all pending signal writes:
1. Updates all signals' values
2. Fires all their value_change events
3. This might wake up processes (Process B)
4. Those processes run in the next delta cycle

This provides deterministic ordering: all processes at delta N see the state from delta N-1, and their writes affect delta N+1.

**Event Propagation Flow**

Here's how events flow through the system:

```
1. User code calls: clk << 1
   ↓
2. Clock's write() method:
   - Stores new value (1)
   - Schedules update for next delta
   ↓
3. Delta cycle update phase:
   - Clock's _update() commits value
   - Detects rising edge (0 → 1)
   - Fires posedge_event
   ↓
4. Event fires:
   - Kernel looks up waiting coroutines
   - Finds Counter._process_wrapper
   - Calls resume() on it
   ↓
5. Counter coroutine wakes:
   - Returns from wait()
   - Executes process()
   - Maybe writes other signals
   ↓
6. Those writes schedule next delta:
   - count << _value
   - Schedules count update
   - May trigger more delta cycles
```

### 2.4 How Coroutines Enable Process Modeling

The mapping from hardware processes to Cxy coroutines is incredibly natural.

**Hardware Process Behavior**

A hardware process (like a flip-flop's behavior) conceptually does:

```
Forever:
  Wait for clock rising edge
  Sample input
  Compute output
  Register output
```

**Coroutine Implementation**

Our generated code literally writes this:

```cxy
async _process_wrapper() {
    while (true) {          // Forever
        wait(clk.posedge()) // Wait for clock rising edge
        process()           // Sample input, compute, output
    }
}
```

**The wait() Magic**

Inside `wait()`:

```cxy
func wait(event: &Event) {
    var coro = running()        // "Get my own coroutine handle"
    event.addWaiter(coro)       // "Add me to event's wait list"
    coro.suspend()              // "Put me to sleep"
    // Execution stops here
    
    // ... time passes, event fires ...
    
    // Execution resumes here when event fires
}
```

The `running()` built-in is key - it lets code get its own coroutine handle from within. We couldn't do this with traditional callback-based async or fibers.

**Why Symmetric Matters**

Cxy's **symmetric** coroutines mean any coroutine can resume any other. The simulation kernel holds coroutine handles and calls `resume()` on them when events fire. We're not limited to caller/callee relationships like stackful coroutines or async/await.

This matches hardware: events are broadcast to all interested parties simultaneously (in parallel). Our kernel can resume multiple coroutines from one event.

### 2.5 Plugin Transformation Pipeline

The plugin processes code in multiple passes because later passes need information from earlier ones.

**Pass 1: Discovery & Validation**

Scan the entire AST to find:
- All structs with `@rtl::component`
- All fields with `@in/@out/@inout`  
- All methods with `@method`
- All test blocks with `@rtl::testbench`

Build a symbol table: "Counter has ports clk, reset, count and method process with trigger clk:posedge".

Validate:
- Are port types valid signal types?
- Do @method triggers reference existing ports?
- Are attributes used in correct contexts?

Report errors now, before transformation.

**Pass 2: Type Transformation**

Transform field types:
- `@in T` → `InPort[T]`
- Keep track of original type T (need it for constructor parameters)

This must happen before generating the constructor because the constructor needs to know parameter types.

**Pass 3: Code Generation**

Generate new code:
- Constructor with binding logic
- Async wrapper for each @method
- Testbench initialization code

These are AST nodes (parsed code structures), not strings.

**Pass 4: Injection**

Insert generated code into the AST:
- Add constructor to struct's methods
- Add wrappers to struct's methods (marked private)
- Preserve user's original methods
- Wrap testbench bodies

The final AST has both user code and generated code, ready for the normal compilation pipeline.

---

## 3. Core Library Design

The runtime library provides the fundamental types and simulation infrastructure. Each type has a specific purpose in modeling hardware behavior. Let's examine each one, understanding not just what it does, but why it's designed that way.

### 3.1 Time: Representing Simulation Time

**The Problem:** Hardware simulation needs to represent time precisely. A clock period might be 10 nanoseconds, a signal delay might be 2.5 nanoseconds, a timeout might be 1 millisecond. We need:
- Multiple time units (ns, us, ms, s)
- Precise comparisons (is 10ns before 15ns?)
- Arithmetic (2 × 5ns = 10ns)
- Conversion between units

**Design Decision:** Store time as an integer value + unit enum. This avoids floating-point imprecision while supporting human-readable durations. Leverage Cxy's literal suffixes and operator overloading for natural syntax.

**Literal Suffix Support:**

Cxy supports literal suffixes that transform at compile time. When you write `10.ns`, the compiler transforms it to `ns(10)`. We define these functions:

```cxy
pub enum TimeUnit {
    Picosecond,
    Nanosecond,
    Microsecond,
    Millisecond,
    Second
}

pub struct Time {
    value: i64
    unit: TimeUnit
    
    // Literal suffix functions - compiler transforms 10.ns → ns(10)
    pub func ns(value: i64): Time => Time(value, TimeUnit.Nanosecond)
    pub func us(value: i64): Time => Time(value, TimeUnit.Microsecond)
    pub func ms(value: i64): Time => Time(value, TimeUnit.Millisecond)
    pub func ps(value: i64): Time => Time(value, TimeUnit.Picosecond)
    pub func s(value: i64): Time => Time(value, TimeUnit.Second)
}
```

**Operator Overloading:**

Time supports natural arithmetic and comparison through operator overloading:

```cxy
pub struct Time {
    // ... fields ...
    
    // Arithmetic operators
    func `+`(other: &Time): Time {
        // Normalize to common unit, add, return new Time
        var ns1 = this.toNanoseconds()
        var ns2 = other.toNanoseconds()
        return Time.fromNanoseconds(ns1 + ns2)
    }
    
    func `-`(other: &Time): Time {
        var ns1 = this.toNanoseconds()
        var ns2 = other.toNanoseconds()
        return Time.fromNanoseconds(ns1 - ns2)
    }
    
    func `*`(scalar: i64): Time {
        return Time(this.value * scalar, this.unit)
    }
    
    func `/`(scalar: i64): Time {
        return Time(this.value / scalar, this.unit)
    }
    
    // Comparison operators
    func `==`(other: &Time): bool {
        return this.toNanoseconds() == other.toNanoseconds()
    }
    
    func `<`(other: &Time): bool {
        return this.toNanoseconds() < other.toNanoseconds()
    }
    
    func `>`(other: &Time): bool {
        return this.toNanoseconds() > other.toNanoseconds()
    }
    
    func `<=`(other: &Time): bool {
        return this.toNanoseconds() <= other.toNanoseconds()
    }
    
    func `>=`(other: &Time): bool {
        return this.toNanoseconds() >= other.toNanoseconds()
    }
    
    // Helper: normalize to nanoseconds for comparison/arithmetic
    func toNanoseconds(): i64 {
        match this.unit {
            TimeUnit.Picosecond => this.value / 1000
            TimeUnit.Nanosecond => this.value
            TimeUnit.Microsecond => this.value * 1000
            TimeUnit.Millisecond => this.value * 1000000
            TimeUnit.Second => this.value * 1000000000
            ... => 0
        }
    }
}
```

**Behavior:**
- Time values are immutable (creating 10.ns returns a Time value)
- Arithmetic creates new Time values
- Comparisons normalize to a common unit (nanoseconds) for accuracy
- Zero-time is representable (important for delta cycles)

**Why This Matters:** Simulation time is not wallclock time. The simulator jumps from event to event - 0ns, 10ns, 25ns, 100ns. Between 10ns and 25ns, no "time" passes in the simulation. Time is discrete points when interesting things happen.

**Usage Example:**
```cxy
// Literal suffixes for natural time creation
var clk_period = 10.ns              // Transformed to ns(10)
var timeout = 1.ms                  // Transformed to ms(1)
var delay = 100.ps                  // Transformed to ps(100)

// Operator overloading for natural arithmetic
var total = clk_period * 100        // 1000ns = 1us
var sum = clk_period + 5.ns         // 15ns
var diff = timeout - 500.us         // 500us

// Comparison works across units
assert total < timeout              // true: 1us < 1ms
assert clk_period == 10.ns          // true
assert delay > 50.ps                // true

// Usage in real code
var clock = Clock("clk", 10.ns)     // 10ns period
ctx.run(100.ns)                     // Run for 100ns
```

**Implementation Notes:** Internally normalize to smallest unit (nanoseconds or picoseconds) for comparison. Consider fixed-point representation if sub-picosecond precision is needed. Arithmetic operators create new Time instances (immutable). The literal suffix transformation happens at compile time, so there's no runtime overhead.

### 3.2 Event: The Synchronization Primitive

**The Problem:** Processes need to wait for things to happen. A counter waits for a clock edge. A FIFO waits until it's not empty. A timeout waits for a timer. How do we model "waiting" in a way that integrates with the simulation kernel?

**Design Decision:** Events are objects that processes can wait on. An Event maintains a list of waiting coroutines. When the Event "fires", all waiting coroutines resume. The simulation kernel orchestrates this.

**Behavior:**
- Call `wait(event)` to suspend until event fires
- Call `event.notify()` to fire immediately (next delta cycle)
- Call `event.notify(delay)` to fire after a time delay
- Multiple processes can wait on the same event
- Events can fire even if no one is waiting (no-op)

**Why This Matters:** Events decouple producers and consumers. The code that fires an event doesn't need to know who's waiting. The code that waits doesn't need to know who will fire it. This models hardware's natural decoupling - a clock generator doesn't "know" about all the flip-flops listening to it.

**The Relationship to Signals:** Signals have an internal value_change event. When a signal's value changes, it fires this event, waking processes that are sensitive to that signal. So events are more primitive than signals - signals use events internally.

**Why Notify Can Be Delayed:** Hardware has propagation delays. An event firing "now" doesn't make sense - events are scheduled into the simulation timeline. `notify()` with no argument schedules for the next delta cycle (zero-time but ordered). `notify(10.ns)` schedules for 10ns in the future.

**Conceptual Model:**
```
Event
  ├─ _waiters: List[Coroutine]
  └─ _scheduled_at: ?Time

When wait() called:
  - Add current coroutine to _waiters
  - Suspend coroutine

When notify() called:
  - Schedule event into simulation timeline
  
When event fires (kernel triggers it):
  - Resume all coroutines in _waiters
  - Clear _waiters list
```

### 3.3 Signal[T]: The Communication Channel

**The Problem:** Hardware components communicate via wires/buses that carry values. A signal needs to:
- Hold a typed value (u32, bool, custom struct, etc.)
- Allow reading (immediately see current value)
- Allow writing (schedule update for next delta)
- Notify when value changes (wake up listeners)

**Design Decision:** Signal[T] is a generic type that wraps a value of type T. Writes are deferred (scheduled) to implement delta-cycle semantics. Reads are immediate.

**The Delta Cycle Contract:**

This is crucial to understand. When you write:
```cxy
signal << newValue
```

The signal does **not** update immediately. Instead:
1. newValue is stored in a "pending" slot
2. A value_change event is scheduled for next delta
3. The current read() still returns the old value

At the end of the delta cycle, the simulation kernel calls `_update()` on all dirty signals:
1. The pending value becomes the current value
2. The value_change event fires
3. Processes waiting on value_change resume in the next delta

**Why Deferred Updates?** Consider two processes running in the same delta:

```
Process A:                    Process B:
x = sig.read()  // 0          y = sig.read()  // 0
sig << 1                      z = x + y        // 0 + 0 = 0
```

Both processes see sig = 0 throughout their execution in delta N, even though Process A writes it. In delta N+1, after updates commit, sig = 1 and the value_change event fires.

This provides determinism: the order that Process A and B execute within a delta doesn't matter - they both see the same snapshot of state.

**Reading vs Writing:**

- `signal.read()` or `signal.get()`: Returns current value immediately. Can be called any time. No side effects.
- `signal.write(v)` or `signal << v`: Schedules value for next delta. Can be called only on writable signals. Side effect: schedules event.

**The << Operator:**

The write operator is implemented as an overloaded operator:

```cxy
pub struct Signal[T] {
    _current: T
    _next: ?T
    _changed: bool
    value_change: Event
    
    func read(): T {
        return _current
    }
    
    // Overloaded << operator for natural write syntax
    func `<<`(value: T): void {
        _next = value
        _changed = true
        // Schedule value_change event for next delta
        SimContext.scheduleUpdate(this)
    }
}
```

**The Value Change Event:**

Every Signal has a `value_change` event. After a signal updates (in the update phase), if the value actually changed (new ≠ old), the value_change event fires. Processes can:

```cxy
wait(data.value_change())    // Wait for data to change
```

This is how combinational processes work - sensitive to input changes, not clocks.

**Design Rationale - Why Generic?**

We make Signal generic (`Signal[T]`) rather than having separate `SignalU32`, `SignalBool`, etc. because:
- Reuses code (one implementation works for all types)
- Type safety (can't accidentally connect Signal[u32] to Signal[bool])
- User-defined types work (Signal[MyStruct])
- Cleaner API (Signal[T].read() returns T, not generic Object)

**Conceptual Model:**
```
Signal[T]
  ├─ _current: T (the visible value)
  ├─ _next: ?T (pending write, if any)
  ├─ _changed: bool (dirty flag)
  └─ value_change: Event

Reading flow:
  read() → return _current

Writing flow:
  write(v) or << v → 
    _next = v
    _changed = true
    schedule value_change event for next delta
    register with SimContext for update

Update flow (called by kernel):
  if (_changed) {
    _current = _next
    _next = null
    _changed = false
    fire value_change event
  }
```

### 3.4 Bit: Four-State Logic

**The Problem:** Digital hardware doesn't just have 0 and 1. There's also:
- **X (unknown)**: Uninitialized, invalid, or contradictory value
- **Z (high-impedance)**: Tri-state bus not driven

We need a signal type that represents these states and provides logical operations.

**Design Decision:** Bit is a specialized signal that wraps a character value representing one of four states. It accepts both character literals (`'0'`, `'1'`, `'X'`, `'Z'`) and integer literals (`0`, `1`) for convenience. Additionally, Bit provides edge detection and bitwise operations via operator overloading.

**Character and Integer Literal Support:**

Bit can be initialized and assigned using either form:

```cxy
// Character literals for all four states
var unknown = Bit("sig", 'X')      // Unknown state
var highz = Bit("tri", 'Z')        // High-impedance
var one = Bit("a", '1')            // Logic high
var zero = Bit("b", '0')           // Logic low

// Integer literals for convenience (binary states only)
var high = Bit("c", 1)             // Equivalent to '1'
var low = Bit("d", 0)              // Equivalent to '0'

// Assigning values
bit << '1'                         // Character form
bit << 1                           // Integer form (equivalent)
bit << 'X'                         // Only character form for X/Z

// Reading values
if bit.read() == 'Z' {             // Compare with character
    // Handle high-impedance
}
if bit.read() == 1 {               // Compare with integer
    // Handle logic high
}
```

**Why Both Forms?**

- **Characters** (`'0'`, `'1'`, `'X'`, `'Z'`): Matches hardware notation (Verilog, VHDL). Natural for all four states. Makes code look like hardware diagrams.
- **Integers** (`0`, `1`): Convenient for binary logic, which is 95% of use cases. Shorter to type. Familiar to software developers.

The library internally converts between them, so both forms work seamlessly.

**Operator Overloading for Bitwise Operations:**

Bit supports natural bitwise operations via operator overloading:

```cxy
pub struct Bit {
    _signal: Signal[wchar]    // Internally stores '0', '1', 'X', or 'Z'
    posedge: Event
    negedge: Event
    
    // Constructor overloads for both forms
    func `init`(name: String, value: wchar) {
        _signal = Signal[wchar](name, value)
    }
    
    func `init`(name: String, value: i32) {
        var char_value = if value == 0 { '0' } else { '1' }
        _signal = Signal[wchar](name, char_value)
    }
    
    // Bitwise AND operator
    func `&`(other: &Bit): Bit {
        var a = this.read()
        var b = other.read()
        var result = match (a, b) {
            ('0', '0') => '0'
            ('0', '1') => '0'
            ('1', '0') => '0'
            ('1', '1') => '1'
            ... => 'X'    // Unknown if either is X/Z
        }
        return Bit("temp", result)
    }
    
    // Bitwise OR operator
    func `|`(other: &Bit): Bit {
        var a = this.read()
        var b = other.read()
        var result = match (a, b) {
            ('0', '0') => '0'
            ('0', '1') => '1'
            ('1', '0') => '1'
            ('1', '1') => '1'
            ('1', 'X') => '1'    // 1 | X = 1
            ('X', '1') => '1'
            ... => 'X'
        }
        return Bit("temp", result)
    }
    
    // Bitwise XOR operator
    func `^`(other: &Bit): Bit {
        var a = this.read()
        var b = other.read()
        var result = match (a, b) {
            ('0', '0') => '0'
            ('0', '1') => '1'
            ('1', '0') => '1'
            ('1', '1') => '0'
            ... => 'X'
        }
        return Bit("temp", result)
    }
    
    // Bitwise NOT operator
    func `~`(): Bit {
        var val = this.read()
        var result = match val {
            '0' => '1'
            '1' => '0'
            ... => 'X'
        }
        return Bit("temp", result)
    }
    
    // Comparison operators
    func `==`(other: wchar): bool {
        return this.read() == other
    }
    
    func `==`(other: i32): bool {
        var val = this.read()
        if other == 0 {
            return val == '0'
        } else {
            return val == '1'
        }
    }
    
    // Write operator (accepts both forms)
    func `<<`(value: wchar): void {
        _signal << value
    }
    
    func `<<`(value: i32): void {
        var char_value = if value == 0 { '0' } else { '1' }
        _signal << char_value
    }
    
    func read(): wchar {
        return _signal.read()
    }
}
```

**Usage Examples:**

```cxy
var a = Bit("a", 1)
var b = Bit("b", 0)
var c = Bit("c", 'X')

// Natural bitwise operations
var and_result = a & b               // '0'
var or_result = a | b                // '1'
var xor_result = a ^ b               // '1'
var not_result = ~a                  // '0'

// Chaining operations
var complex = a & b | c              // '0' | 'X' = 'X'
var inverted = ~(a & b)              // ~'0' = '1'

// Assignment with both forms
a << 0                               // Integer form
b << '1'                             // Character form
c << 'Z'                             // High-impedance (character only)

// Comparison with both forms
if a.read() == 1 { }                 // Integer comparison
if b.read() == '1' { }               // Character comparison
if c.read() == 'Z' { }               // High-Z check
```

**Four-State Logic Truth Tables:**

The bitwise operations implement proper four-state logic:

**AND (`&`):**
```
   | '0'  '1'  'X'  'Z'
---|--------------------
'0'| '0'  '0'  '0'  '0'
'1'| '0'  '1'  'X'  'X'
'X'| '0'  'X'  'X'  'X'
'Z'| '0'  'X'  'X'  'X'
```

**OR (`|`):**
```
   | '0'  '1'  'X'  'Z'
---|--------------------
'0'| '0'  '1'  'X'  'X'
'1'| '1'  '1'  '1'  '1'
'X'| 'X'  '1'  'X'  'X'
'Z'| 'X'  '1'  'X'  'X'
```

**NOT (`~`):**
```
'0' → '1'
'1' → '0'
'X' → 'X'
'Z' → 'X'
```

**Edge Detection:**

Clocks and other control signals care about **transitions**, not just values. A clock doesn't just "be 1", it **transitions from 0 to 1** (positive edge).

Bit maintains:
- `posedge_event`: Fires on 0 → 1 transition
- `negedge_event`: Fires on 1 → 0 transition
- `anyedge_event`: Fires on any transition

After each signal update, Bit compares old and new values. If it's a rising edge, fire posedge_event. This enables:

```cxy
wait(clk.posedge())    // Precise edge sensitivity
```

**Four-State Logic Operations:**

When you AND two Bits, what should `1 & X` be? The IEEE 1364 standard defines this:
- `0 & anything` = 0 (definite)
- `1 & 1` = 1 (definite)
- `1 & X` = X (unknown)
- `X & X` = X (unknown)
- etc.

Our Bit type implements these rules so simulations can model real hardware's X propagation.

**Why Not Just bool?**

A bool can only be true/false. It can't represent "I don't know" (X) or "nothing is driving this" (Z). Hardware simulation needs these states to catch bugs:
- Reading an uninitialized register → X
- Multiple drivers on a bus in conflict → X
- Tri-state bus with no driver → Z

**Conceptual Model:**
```
Bit
  ├─ _signal: Signal[BitValue]
  ├─ _last_value: BitValue (for edge detection)
  ├─ posedge_event: Event
  ├─ negedge_event: Event
  └─ anyedge_event: Event

On update:
  new_val = _signal.read()
  if (new_val != _last_value) {
    if (_last_value == Zero && new_val == One)
      fire posedge_event
    else if (_last_value == One && new_val == Zero)
      fire negedge_event
    fire anyedge_event
    _last_value = new_val
  }
```

### 3.5 Bus[N]: Fixed-Width Vectors

**The Problem:** Many hardware signals are multi-bit buses - address buses (32 bits), data buses (64 bits), control fields (8 bits). We need a type that:
- Has fixed width (known at compile time)
- Supports bit indexing (get/set individual bits)
- Supports arithmetic and bitwise operations
- Maintains four-state logic per bit

**Design Decision:** Bus[N] is a compile-time parameterized type where N is the bit width. Internally it's an array of Bit values, with operations defined over the whole vector. Uses Cxy's operator overloading for natural indexing syntax.

**Fixed Width at Compile Time:**

The width is part of the type: `Bus[32]` is different from `Bus[16]`. The compiler knows the width, can validate operations (can't mix different widths), and can optimize accordingly.

**Index Operator Overloading:**

Bus uses Cxy's `[]` and `=[]` operator overloading for natural bit access. **Important:** In Cxy, array indexing uses dot notation: `bus.[3]` not `bus[3]`.

```cxy
pub struct Bus[N] {
    _bits: [Bit; N]    // Array of N bits
    
    // Index operator - returns bit at position
    func `[]`(index: i32): Bit {
        return _bits.[index]
    }
    
    // Index assignment operator - sets bit at position
    func `=[]`(index: i32, value: Bit): void {
        _bits.[index] = value
    }
    
    // Overload for character values
    func `=[]`(index: i32, value: wchar): void {
        _bits.[index] << value
    }
    
    // Overload for integer values (0/1)
    func `=[]`(index: i32, value: i32): void {
        _bits.[index] << value
    }
}
```

**Usage Examples:**

```cxy
// Create 32-bit bus
var addr = Bus[32]("addr")
var data = Bus[8]("data")

// Index access (note the dot!)
var bit5 = addr.[5]              // Get bit at index 5
var msb = data.[7]               // Get most significant bit

// Index assignment (supports characters and integers)
addr.[0] = '1'                   // Set bit 0 using character
addr.[1] = 1                     // Set bit 1 using integer
addr.[2] = 'X'                   // Set bit 2 to unknown
data.[7] = '0'                   // Clear MSB

// Reading individual bits
if addr.[0].read() == 1 {
    // Bit 0 is high
}

if data.[7].read() == 'Z' {
    // MSB is high-impedance
}
```

**Bitwise Operator Overloading:**

Bus supports natural bitwise operations via operator overloading:

```cxy
pub struct Bus[N] {
    // Bitwise AND
    func `&`(other: &Bus[N]): Bus[N] {
        var result = Bus[N]("temp")
        for i in 0..N {
            result.[i] = this._bits.[i] & other._bits.[i]
        }
        return result
    }
    
    // Bitwise OR
    func `|`(other: &Bus[N]): Bus[N] {
        var result = Bus[N]("temp")
        for i in 0..N {
            result.[i] = this._bits.[i] | other._bits.[i]
        }
        return result
    }
    
    // Bitwise XOR
    func `^`(other: &Bus[N]): Bus[N] {
        var result = Bus[N]("temp")
        for i in 0..N {
            result.[i] = this._bits.[i] ^ other._bits.[i]
        }
        return result
    }
    
    // Bitwise NOT
    func `~`(): Bus[N] {
        var result = Bus[N]("temp")
        for i in 0..N {
            result.[i] = ~this._bits.[i]
        }
        return result
    }
    
    // Equality comparison
    func `==`(other: &Bus[N]): bool {
        for i in 0..N {
            if this._bits.[i].read() != other._bits.[i].read() {
                return false
            }
        }
        return true
    }
}
```

**Usage Examples:**

```cxy
var a = Bus[8]("a")
var b = Bus[8]("b")
var mask = Bus[8]("mask")

// Natural bitwise operations
var and_result = a & b           // AND operation
var or_result = a | b            // OR operation
var xor_result = a ^ b           // XOR operation
var not_result = ~a              // NOT operation

// Combining operations
var masked = a & mask            // Apply mask
var inverted = ~(a | b)          // Invert OR result

// Setting multiple bits
for i in 0..8 {
    a.[i] = if i % 2 == 0 { 1 } else { 0 }
}

// Reading multiple bits
for i in 0..8 {
    if a.[i].read() == 'X' {
        println("Bit {i} is unknown")
    }
}
```

**Arithmetic Operations (Optional):**

Bus can also support arithmetic if treating bits as unsigned integer:

```cxy
pub struct Bus[N] {
    func toU64(): u64 {
        var result: u64 = 0
        for i in 0..N {
            if this._bits.[i].read() == '1' {
                result |= (1 << i)
            }
        }
        return result
    }
    
    func `+`(other: &Bus[N]): Bus[N] {
        var sum = this.toU64() + other.toU64()
        return Bus[N].fromU64(sum)
    }
}
```

**Why Separate from Signal?**

Bus[N] is not a Signal - it's a value type. You put a Bus into a Signal:
```cxy
var addr_signal = Signal[Bus[32]]("addr", Bus[32]())
```

This separates "multi-bit value" from "time-varying signal". A Bus can exist outside simulation (in testbench computations). A Signal is always part of simulation.

**Important Syntax Notes:**

1. **Generic syntax:** `Bus[N]` with square brackets (Cxy uses square brackets for generics, not angle brackets)
2. **Array indexing:** `bus.[index]` not `bus[index]` (Cxy uses dot notation)
3. **Operator overloading:** Enables `bus.[3]` syntax via `` func `[]`(index: i32) ``

**Conceptual Model:**
```
Bus[N]
  └─ _bits: [Bit; N] (array of N bits)

Index access:
  bus.[i] → calls `[]` operator → returns _bits.[i]

Index assignment:
  bus.[i] = value → calls `=[]` operator → sets _bits.[i]

Bitwise operations:
  bus1 & bus2 → calls `&` operator → bit-by-bit AND

Arithmetic (optional):
  bus + 1 → converts to u64, adds, converts back
```

### 3.6 Clock: Periodic Signal with Auto-Generation

**The Problem:** Almost every hardware model needs a clock. Clocks are tedious to write manually:

```cxy
// Don't want to write this every time:
async generate_clock() {
    while (true) {
        clk << 0
        wait_for(5.ns)
        clk << 1
        wait_for(5.ns)
    }
}
```

We need a type that encapsulates periodic toggle with configurable period and duty cycle.

**Design Decision:** Clock is a special signal that auto-generates edges. It wraps a Bit and runs an internal coroutine that toggles the bit at regular intervals.

**Automatic Generation:**

When you create a Clock and call `start()`, it launches an internal `async _generate()` coroutine:

```
Loop forever:
  Set bit to 1
  Sleep for high_time (period × duty_cycle)
  Set bit to 0
  Sleep for low_time (period × (1 - duty_cycle))
```

This coroutine runs throughout simulation, generating edges automatically. No user code needed.

**Why It's Still a Bit:**

Clock wraps a Bit, so you get all of Bit's edge events:
```cxy
var clk = Clock("clk", 10.ns)
clk.start()

wait(clk.posedge())    // Uses underlying Bit's posedge_event
```

Clock just adds the automatic generation behavior on top of Bit's signal and event capabilities.

**Duty Cycle:**

Most clocks are 50% duty cycle (high for half the period, low for half). But some designs need different duty cycles. Clock supports this:

```cxy
var clk = Clock("clk", 10.ns, duty_cycle: 0.6)
// High for 6ns, low for 4ns, period still 10ns
```

**Start/Stop Control:**

Clocks can be paused:
```cxy
clk.start()     // Begin toggling
clk.stop()      // Pause toggling
clk.reset()     // Reset to initial state
```

This is useful for power-down scenarios or gated clocks.

**Why Not Just Use Signal[Bit]?**

You could manually write clock generation, but Clock provides:
- Less boilerplate (one line vs a whole async function)
- Standard interface (every Clock has start/stop/period)
- Automatic cleanup (internal coroutine managed by Clock)
- Common patterns built-in (duty cycle, phase offset)

**Conceptual Model:**
```
Clock
  ├─ _bit: Bit (the actual signal)
  ├─ _period: Time
  ├─ _duty_cycle: f64
  ├─ _running: bool
  └─ _generate_coro: Coroutine (internal)

On start():
  _running = true
  launch async _generate()

async _generate():
  while (_running) {
    _bit << 1
    wait_for(_period * _duty_cycle)
    _bit << 0
    wait_for(_period * (1 - _duty_cycle))
  }
```

### 3.7 Ports: InPort[T], OutPort[T], InOutPort[T]

**The Problem:** A component's fields are not the signals themselves - they're **connections** to external signals. Consider:

```cxy
struct Adder {
    a: ???          // Input connection
    b: ???          // Input connection
    sum: ???        // Output connection
}
```

What type should these be? They're not `Signal[u32]` - the Adder doesn't own the signals, it just connects to them. We need types that represent "connection points" with direction semantics.

**Design Decision:** Ports are wrapper types that hold a reference to an external signal and enforce access restrictions based on direction.

**InPort[T]**: Input port, read-only. Can call `read()`, cannot call `write()`. Wraps a `&Signal[T]`.

**OutPort[T]**: Output port, write-only. Can call `write()` or `<<`, cannot call `read()`. Wraps a `&Signal[T]`.

**InOutPort[T]**: Bidirectional port, read-write. Can do both. Used for bidirectional buses. Wraps a `&Signal[T]`.

**Port Type Definitions:**

```cxy
pub struct InPort[T] {
    _signal: ?&Signal[T]
    
    func bind(signal: &Signal[T]): void {
        _signal = signal
    }
    
    func read(): T {
        // Validation: ensure bound before use
        if _signal == null {
            panic("InPort not bound before read")
        }
        return _signal.read()
    }
    
    func valueChange(): &Event {
        return _signal.value_change
    }
    
    // No write() method - enforced at compile time
}

pub struct OutPort[T] {
    _signal: ?&Signal[T]
    
    func bind(signal: &Signal[T]): void {
        _signal = signal
    }
    
    func `<<`(value: T): void {
        if _signal == null {
            panic("OutPort not bound before write")
        }
        _signal << value
    }
    
    func write(value: T): void {
        this << value
    }
    
    // No read() method - enforced at compile time
}

pub struct InOutPort[T] {
    _signal: ?&Signal[T]
    
    func bind(signal: &Signal[T]): void {
        _signal = signal
    }
    
    func read(): T {
        if _signal == null {
            panic("InOutPort not bound before read")
        }
        return _signal.read()
    }
    
    func `<<`(value: T): void {
        if _signal == null {
            panic("InOutPort not bound before write")
        }
        _signal << value
    }
    
    func valueChange(): &Event {
        return _signal.value_change
    }
}
```

**Binding Semantics:**

Ports start unbound (not connected to anything). The component's constructor (generated by plugin) binds them:

```cxy
func `init`(a: &Signal[u32], b: &Signal[u32], sum: &Signal[u32]) {
    this.a.bind(a)      // Connect port to external signal
    this.b.bind(b)
    this.sum.bind(sum)
}
```

After binding, the port transparently forwards operations to the underlying signal:
```cxy
var val = this.a.read()     // Reads from bound signal
this.sum << result          // Writes to bound signal
```

**Why Not Just &Signal[T]?**

We could make all ports `&Signal[T]`, but then:
- No direction enforcement (could accidentally write to input)
- No binding validation (could forget to bind)
- No semantic clarity (is this input or output?)

Ports provide these guarantees at compile time.

**Transparent Access:**

Once bound, ports act like the underlying signal:
```cxy
this.a.read()                       // InPort's read() calls signal's read()
this.sum << 42                      // OutPort's << calls signal's write()
wait(this.data.valueChange())       // InPort provides access to events
```

The port is just a thin wrapper for type safety.

**Validation:**

Ports can validate they're bound before use:
```cxy
func read(): T {
    if (_signal.isNone()) {
        raise "Port not bound"
    }
    return _signal.unwrap().read()
}
```

This catches bugs where a component is used without proper initialization.

**Why Three Types?**

Most ports are unidirectional (input or output). Having separate InPort/OutPort types lets the compiler catch mistakes like writing to an input. InOutPort is needed for bidirectional buses (e.g., memory data bus that goes both directions).

**Conceptual Model:**
```
InPort[T]
  ├─ _signal: ?&Signal[T] (optional reference, bound later)
  └─ Operations: read(), valueChange()

OutPort[T]
  ├─ _signal: ?&Signal[T]
  └─ Operations: write(), <<

InOutPort[T]
  ├─ _signal: ?&Signal[T]
  └─ Operations: read(), write(), <<, valueChange()

Binding:
  bind(sig: &Signal[T]) → _signal = sig

Access:
  read() → _signal.unwrap().read()
  write(v) → _signal.unwrap().write(v)
```

### 3.8 SimContext: The Simulation Kernel

**The Problem:** All the pieces we've described - signals, events, coroutines - need coordination. Something needs to:
- Track current simulation time
- Maintain the event queue (when do things happen?)
- Execute delta cycles (update signals, fire events, repeat until stable)
- Manage coroutine lifecycle (who's waiting for what?)
- Provide the `wait()` function that processes call

**Design Decision:** SimContext is the central simulation kernel. It's a global context that components and processes interact with. One SimContext per simulation.

**Responsibilities:**

1. **Time Management**: Knows current simulation time, advances time from event to event.

2. **Event Scheduling**: Maintains a priority queue of future events ordered by time.

3. **Delta Cycle Execution**: Runs evaluate-update-check cycles until stable at each time point.

4. **Coroutine Tracking**: Knows which coroutines are waiting on which events, resumes them when events fire.

5. **Signal Update Coordination**: Collects dirty signals, updates them in the update phase.

**The Main Simulation Loop:**

SimContext's core is the simulation loop:

```
while (events remain and time < limit) {
    1. Pop next event from queue (earliest time)
    2. Advance simulation time to that event's time
    3. Add event to delta-cycle queue
    4. Run delta cycles until stable:
       a. Evaluation phase: fire all delta events, resume waiting processes
       b. Update phase: commit all pending signal writes
       c. Check: any new delta events? If yes, repeat
    5. Continue to next event
}
```

This implements SystemC's semantics exactly.

**The wait() Function:**

This is the most important function processes call:

```cxy
func wait(event: &Event) {
    var coro = running()           // Magic: get my own coroutine
    _registerWaiter(event, coro)   // Add me to event's wait list
    coro.suspend()                 // Put me to sleep
    // Execution stops here
    
    // ... time passes ...
    
    // Execution resumes here when event fires
}
```

This is how processes block. When a process calls `wait(clk.posedge())`, it suspends itself and registers interest in the posedge event. Later, when the posedge event fires, SimContext resumes that coroutine.

**Delta Cycle Details:**

Delta cycles are sub-steps at the same simulation time. Why do we need them?

Example: Process A writes signal X, which triggers Process B, which writes signal Y, which triggers Process C.

All of this happens "at 10ns" but needs ordering. Delta cycles provide that:
- Delta 0: Process A runs, schedules X update
- Delta 1: X updates, fires value_change, Process B runs, schedules Y update
- Delta 2: Y updates, fires value_change, Process C runs
- Delta 3: No more updates, advance time

Each delta is a complete evaluate-update cycle. We iterate until no new updates occur (stable state).

**Why It's a Singleton (conceptually):**

Typically there's one SimContext per simulation run. Components don't own it, they reference it. The testbench creates it:

```cxy
var ctx = SimContext()
var counter = Counter(&ctx, ...)
ctx.run(100.ns)
```

**Conceptual Model:**
```
SimContext
  ├─ _current_time: Time
  ├─ _event_queue: PriorityQueue[Event, Time]
  ├─ _delta_events: Queue[Event]
  ├─ _wait_map: Map[Event, List[Coroutine]]
  ├─ _dirty_signals: Set[&Signal]
  └─ _running: bool

Simulation loop:
  while (_running && !_event_queue.empty()) {
    event, time = _event_queue.pop()
    _current_time = time
    _delta_events.push(event)
    
    delta = 0
    loop {
      // Evaluate
      for event in _delta_events {
        for coro in _wait_map[event] {
          coro.resume()
        }
        _wait_map.remove(event)
      }
      _delta_events.clear()
      
      // Update
      for sig in _dirty_signals {
        sig._update()
      }
      _dirty_signals.clear()
      
      // Check
      if _delta_events.empty() break
      delta += 1
      if delta > 1000 raise "Delta overflow"
    }
  }
```

---

## 4. Plugin System Design

The plugin is where the magic happens. It transforms clean, declarative user code into the verbose, boilerplate-heavy code needed to integrate with the simulation kernel. Understanding how the plugin works helps appreciate why the user-facing API is so clean.

### 4.1 The Plugin's Role: Code Generation vs Interpretation

There are two ways to implement attributes like `@rtl::component`:

**Runtime Interpretation:** At runtime, scan objects for attributes, configure them dynamically. This is how Java annotations or Python decorators often work.

**Compile-Time Transformation:** At compile time, see the attributes in the AST, generate code, inject it. The attributes are gone by runtime - only generated code remains.

We choose **compile-time transformation** because:

1. **Zero Runtime Overhead:** No reflection, no runtime attribute scanning, no dynamic dispatch. The generated code is as efficient as hand-written code.

2. **Early Error Detection:** Mistakes are caught at compile time with clear messages, not runtime crashes.

3. **Better Tooling:** IDEs see the generated code, debuggers step through it, stack traces show it.

4. **Type Safety:** The compiler type-checks generated code, ensuring it's valid Cxy.

### 4.2 How Plugins Work in Cxy

A Cxy plugin has full access to the Abstract Syntax Tree (AST) - the parsed representation of source code. The compiler:

1. Parses source files into AST
2. Invokes registered plugins
3. Plugins transform the AST
4. Compiler continues with transformed AST

Plugins can:
- Add new declarations (functions, fields, types)
- Modify existing declarations (change types, add statements)
- Validate constraints (check that attributes are used correctly)
- Report errors (compile will fail if plugin reports errors)

Our RTL plugin registers handlers for specific attributes (`@rtl::component`, `@in`, etc.). When the compiler sees these, it calls our handlers with the relevant AST nodes.

### 4.3 @rtl::component: Transforming Structs into Components

This is the most complex transformation. Let's trace through what happens when the plugin sees:

```cxy
@rtl::component
struct Counter {
    @in clk: Clock
    @in reset: Bit
    @out count: u32
    
    _value: u32 = 0
    
    @method(clk: "posedge")
    func process() {
        if (reset.read() == 1) {
            _value = 0
        } else {
            _value += 1
        }
        count << _value
    }
}
```

**Step 1: Validation**

Before transforming anything, validate:

- Is this actually a struct? (Can't apply @rtl::component to enum or function)
- Are there any `@in/@out/@inout` fields? (Component should have ports)
- Are port field types valid? (Must be signal types like Clock, Bit, or base types we can wrap)
- Are there any `@method` functions? (Components typically have processes)
- Do `@method` trigger references exist? (If @method(clk: "posedge"), does field 'clk' exist?)

If validation fails, report clear errors and stop. Don't generate broken code.

**Step 2: Discover Ports**

Scan all fields looking for `@in`, `@out`, `@inout` attributes. For each port found:

```
Port: clk
  Direction: Input
  User-written type: Clock
  
Port: reset
  Direction: Input
  User-written type: Bit
  
Port: count
  Direction: Output
  User-written type: u32
```

Store this information - we'll need it to generate the constructor.

**Step 3: Transform Port Field Types**

This is subtle but important. The user writes:

```cxy
@in clk: Clock
```

We need to transform this to:

```cxy
clk: InPort[Clock]
```

Why? Because the component doesn't own the clock - it has a connection to an external clock. InPort provides that "connection to external signal" semantics.

But we can't just blindly replace the type. We need to remember the original type (Clock) because the constructor will take `&Clock` as a parameter, not `&InPort[Clock]`.

So the transformation is:
- AST field type: Clock → InPort[Clock]
- Stored metadata: original type = Clock (for constructor generation)

Special case handling:
- `Clock`, `Bit`, `Bus[N]` stay as-is in the generic (InPort[Clock], not InPort[Signal[Clock]])
- Plain types like `u32` become InPort[u32] (conceptually Signal[u32] inside)

**Step 4: Generate Constructor**

Now we generate the constructor function that will be added to the struct. The constructor needs to:

1. Take a reference to a signal for each port
2. Bind each port to its corresponding signal
3. Launch async wrappers for each @method

The parameter list comes from the ports we discovered:

```
Parameters:
  clk: &Clock        (from @in clk: Clock)
  reset: &Bit        (from @in reset: Bit)
  count: &Signal[u32] (from @out count: u32, wraps in Signal)
```

Generate constructor body:

```cxy
func init(clk: &Clock, reset: &Bit, count: &Signal[u32]) {
    // Bind each port
    this.clk.bind(clk)
    this.reset.bind(reset)
    this.count.bind(count)
    
    // Launch processes
    async _process_wrapper()
}
```

The binding calls connect the ports to external signals. The async launch starts the process coroutine.

**Why Generate a Constructor?**

The user didn't write one. We could require users to write constructors manually:

```cxy
func init(clk: &Clock, reset: &Bit, count: &Signal[u32]) {
    this.clk.bind(clk)    // Tedious
    this.reset.bind(reset)
    this.count.bind(count)
    async _process_wrapper()
}
```

But this is pure boilerplate - every component does the exact same thing. The plugin generates it so users don't have to.

**Step 5: Generate Process Wrappers**

For each `@method` function, generate an async wrapper. The wrapper:
- Runs forever in a loop
- Waits for the trigger event
- Calls the user's method

For `@method(clk: "posedge")`:

```cxy
private async _process_wrapper() {
    while (true) {
        wait(this.clk.posedge())
        process()
    }
}
```

Breaking this down:
- `private`: Users don't call this directly
- `async`: It's a coroutine
- `while (true)`: Hardware processes run forever
- `wait(this.clk.posedge())`: Suspend until clock rising edge
- `process()`: Call the user's method

The trigger parsing is key. From `clk: "posedge"` we:
1. Extract signal name: "clk"
2. Extract event type: "posedge"
3. Generate event expression: `this.clk.posedge()`
4. Generate wait call: `wait(this.clk.posedge())`

Different trigger types generate different waits:
- `clk: "posedge"` → `wait(this.clk.posedge())`
- `clk: "negedge"` → `wait(this.clk.negedge())`
- `data: "change"` → `wait(this.data.valueChange())`

**Step 6: Inject Generated Code**

Finally, inject all generated code into the struct's AST:

- Add constructor as a new method
- Add wrapper functions as new private methods
- Keep user's original methods unchanged

The result is a valid Cxy struct with both user and generated code. To a human reading the source, only the user's code is visible. But the compiler sees both.

### 4.4 @in, @out, @inout: Port Field Transformation

These attributes mark struct fields as ports. The plugin handles them as part of `@rtl::component` processing, but let's understand the design decisions.

**Why Three Separate Attributes?**

We could have one attribute: `@port(direction="in")`. But separate attributes are clearer:

```cxy
@in clk: Clock       // Obviously input
@out count: u32      // Obviously output
@inout bus: Bus[8]   // Obviously bidirectional
```

The attribute name documents the intent. It's self-documenting code.

**Type Transformation Rules**

The transformation depends on direction and base type:

```
@in Clock        → InPort[Clock]
@in Bit          → InPort[Bit]
@in Bus[32]      → InPort[Bus[32]]
@in u32          → InPort[u32]

@out u32         → OutPort[u32]
@out Bus[8]      → OutPort[Bus[8]]

@inout Bus[16]   → InOutPort[Bus[16]]
```

Why these rules?
- Specialized types (Clock, Bit) stay as-is, just wrapped in port
- Plain types (u32, bool) are conceptually Signal[T] internally
- Direction determines port type (In/Out/InOut)

**Validation Rules**

The plugin validates:

1. Port attributes only on fields (not functions, not standalone)
2. Port attributes only in @rtl::component structs (not regular structs)
3. Port types are signal-compatible (not arbitrary types)
4. Direction makes sense for usage (can't write to @in, can't read from @out)

Validation happens at compile time, catching mistakes early.

**Why Not Infer Direction?**

Could we infer direction from usage? If a field is only read, it's input; if only written, it's output?

No, because:
- Requires whole-program analysis (what if unused?)
- Less explicit (intent not clear from declaration)
- Error-prone (accidental read of output doesn't change its direction)

Explicit attributes are clearer and safer.

### 4.5 @method(trigger): Process Generation

This attribute transforms a method into a hardware process. The design challenge: how do we specify when the process should run?

**Trigger Specification Syntax**

We support several trigger forms:

```cxy
@method(clk: "posedge")           // Single edge trigger
@method(clk: "negedge")           // Single edge trigger
@method(data: "change")           // Signal change trigger
@method(data: "change", enable: "change")  // Multiple triggers (OR)
```

The syntax is: `signal_name: "event_type"`. Multiple triggers mean "wake up if any fire" (OR sensitivity).

**Why This Syntax?**

Alternative designs:
- `@method(trigger="clk.posedge")` - requires parsing string
- `@method(sensitive=[clk.posedge])` - requires runtime evaluation
- `@method(clk)` - what event? Rising? Falling? Change?

Our syntax is:
- Clear: signal name and event type are explicit
- Parseable: plugin can easily extract parts
- Extensible: can add new event types
- Typed: compiler knows "posedge" is valid for Clock/Bit

**Generating the Wrapper**

For a single trigger:

```cxy
@method(clk: "posedge")
func compute() { ... }
```

Generate:

```cxy
private async _compute_wrapper() {
    while (true) {
        wait(this.clk.posedge())
        compute()
    }
}
```

The wrapper name is derived from the method name: `_{method}_wrapper`. It's private because users don't call it.

**Multiple Triggers (OR Sensitivity)**

For:

```cxy
@method(data: "change", enable: "change")
func combinational() { ... }
```

Generate:

```cxy
private async _combinational_wrapper() {
    while (true) {
        waitAny([this.data.valueChange(), this.enable.valueChange()])
        combinational()
    }
}
```

`waitAny()` is a library function that suspends until **any** of the events fire. It's like OR sensitivity - the process wakes if data changes **or** enable changes.

**Why Preserve the Original Method?**

Notice we generate a wrapper but **keep** the user's original method:

```cxy
// Generated
private async _compute_wrapper() {
    while (true) {
        wait(this.clk.posedge())
        compute()    // Calls user's method
    }
}

// User's original, unchanged
@method(clk: "posedge")
func compute() {
    result << input.read() * 2
}
```

Why not transform the method in-place? Because:

1. **Separation of concerns:** Wrapper handles process mechanics (wait, loop), user method handles logic
2. **Debuggability:** Stack traces show `compute()`, not `_compute_wrapper()`
3. **Testability:** Can call `compute()` directly in tests without simulation
4. **Clarity:** User's code remains intact, readable

The `@method` attribute stays on the original as documentation.

**Trigger Type Validation**

The plugin validates trigger types against signal types:

- "posedge"/"negedge" only valid for Clock or Bit (edge-sensitive signals)
- "change" valid for any signal (value_change event)
- Custom events valid if the type provides them

This catches mistakes like `@method(counter: "posedge")` where counter is u32 (no edges).

### 4.6 @rtl::testbench: Test Wrapper Generation

Testbenches need setup boilerplate: create SimContext, start clocks, provide simulation control. This attribute automates it.

**The Problem with Manual Testbenches**

Without the attribute, testbenches are verbose:

```cxy
test "Counter" {
    var ctx = SimContext()              // Boilerplate
    var clk = Clock("clk", 10.ns)
    clk.start()                         // Boilerplate
    var reset = Bit("reset", 0)
    var count = Signal[u32]("count", 0)
    
    var counter = Counter(&clk, &reset, &count)
    
    reset << 1
    ctx.run(20.ns)                      // Manual simulation control
    assert count.read() == 0
    
    reset << 0
    ctx.run(20.ns)
    assert count.read() == 2
    
    clk.stop()                          // Cleanup boilerplate
}
```

Much of this is the same for every test. The attribute automates it.

**What @rtl::testbench Does**

When the plugin sees:

```cxy
@rtl::testbench
test "Counter" { ... }
```

It transforms the test body:

1. **Inject SimContext creation** at the start
2. **Auto-start clocks** after they're created
3. **Transform simulation control calls** (rtl::simulate → ctx.run)
4. **Add cleanup** at the end

**Step 1: Inject SimContext**

Add at the beginning of the test:

```cxy
var ctx = SimContext()
```

Now the test has a simulation context that components can use.

**Step 2: Auto-Start Clocks**

Scan for Clock variable declarations:

```cxy
var clk = Clock("clk", 10.ns)
```

Immediately after each, inject:

```cxy
clk.start()
```

Clocks need to be running to generate edges. Auto-starting saves repetition.

**Step 3: Transform Simulation Control**

Replace calls to pseudo-functions with actual simulation control:

```cxy
rtl::simulate(20.ns)    →    ctx.run(20.ns)
rtl::advance(10.ns)     →    ctx.run(ctx.now() + 10.ns)
```

These pseudo-functions don't exist in the library - they're markers that the plugin replaces.

Why not use `ctx.run()` directly? Because then every test needs to create and name the ctx variable. With `rtl::simulate()`, tests are more uniform.

**Step 4: Add Cleanup**

At the end of the test, inject:

```cxy
// Stop all clocks
clk.stop()

// Print summary
println("Test completed at ", ctx.now())
```

This ensures clean shutdown and provides diagnostic info.

**Why Transform Tests?**

Tests are where boilerplate is most annoying. Every test does the same setup. By generating it, we:
- Reduce copy-paste errors (forgot to start clock)
- Ensure uniform test structure
- Make tests more readable (focus on logic, not setup)

**Optional Features**

The testbench attribute could support options:

```cxy
@rtl::testbench(trace="counter.vcd")
test "Counter" { ... }
```

The plugin could inject waveform tracing setup, generating VCD files for debugging.

### 4.7 Multi-Pass Plugin Architecture

The plugin processes code in multiple passes because later passes need information from earlier ones. Understanding the pass structure clarifies the implementation strategy.

**Pass 1: Discovery**

Walk the AST looking for attributed declarations:

```
Found:
  - struct Counter with @rtl::component
  - field clk with @in
  - field reset with @in
  - field count with @out
  - function process with @method(clk: "posedge")
  - test "Counter" with @rtl::testbench
```

Build a symbol table/database of what exists and where. This enables later passes to look up information.

**Pass 2: Validation**

Check all constraints:

- Attributes used in correct contexts
- Referenced symbols exist
- Types are compatible
- No conflicting specifications

Report all errors found. Don't proceed if there are errors - generating code from invalid input would just create more confusing errors.

**Pass 3: Type Transformation**

Transform field types based on attributes:

```
Counter.clk: Clock → InPort[Clock]
Counter.reset: Bit → InPort[Bit]
Counter.count: u32 → OutPort[u32]
```

This must happen before code generation because generated code references these new types.

**Pass 4: Code Generation**

Generate all new code:

- Constructors for components
- Async wrappers for processes
- Testbench initialization

Generate as AST nodes, not strings. This ensures generated code is syntactically valid.

**Pass 5: Code Injection**

Insert generated code into the AST:

- Add methods to structs
- Wrap test bodies
- Preserve source locations for error reporting

The result is a transformed AST ready for the rest of compilation.

**Why Multiple Passes?**

We could try to do everything in one pass, but that's fragile:

- Can't validate references before discovering what exists
- Can't generate code referencing new types before transforming types
- Error recovery is harder (one error aborts entire pass)

Multiple passes provide clean separation and better error handling.

### 4.8 Error Messages: Making Compilation Failures Helpful

A key responsibility of the plugin is reporting **clear, actionable errors** when something is wrong. Bad error messages make plugins frustrating to use.

**Example: Invalid Port Type**

User writes:

```cxy
@rtl::component
struct Bad {
    @in data: MyCustomType
}
```

Bad error: "Type error in Bad"

Good error:
```
error: Invalid port type
  --> demo.cxy:3:15
   |
 3 |     @in data: MyCustomType
   |               ^^^^^^^^^^^^ MyCustomType cannot be used as a port type
   |
   = note: Port types must be Signal types (Signal[T], Clock, Bit, Bus[N]) or
           primitive types (u32, bool, etc.)
   = help: If MyCustomType represents signal data, use: @in data: Signal[MyCustomType]
```

The good error:
- Shows exact location (file:line:column)
- Explains what's wrong (can't use as port type)
- Provides context (what types ARE valid)
- Suggests a fix (wrap in Signal)

**Example: Undefined Trigger Reference**

User writes:

```cxy
@method(clk_wrong: "posedge")
func process() { }
```

Good error:
```
error: Undefined signal in trigger
  --> demo.cxy:5:9
   |
 5 |     @method(clk_wrong: "posedge")
   |             ^^^^^^^^^ signal 'clk_wrong' not found in this component
   |
   = note: Available signals: clk, reset, count
   = help: Did you mean 'clk'?
```

Suggest alternatives, show what exists. Help the user fix the problem quickly.

**Example: Invalid Trigger Type**

User writes:

```cxy
@method(counter: "posedge")  // counter is u32, not Clock/Bit
```

Good error:
```
error: Invalid trigger type for signal
  --> demo.cxy:6:18
   |
 6 |     @method(counter: "posedge")
   |                      ^^^^^^^^^ cannot use 'posedge' trigger on type u32
   |
   = note: Edge triggers (posedge/negedge) require Clock or Bit signals
   = help: For u32 signals, use value change trigger: @method(counter: "change")
```

Explain why it's wrong, suggest the correct approach.

**Philosophy of Good Errors**

Every error should answer:
1. **What** is wrong (syntax? type? logic?)
2. **Where** exactly (specific code location)
3. **Why** it's wrong (violates what rule?)
4. **How** to fix it (suggestion or example)

Users shouldn't need to guess or search documentation. The error message should teach them.

---

## 5. Simulation Kernel

The simulation kernel is the engine that makes everything run. It coordinates time, events, and coroutines to create the illusion of concurrent hardware execution. Understanding the kernel's algorithms is key to understanding simulation behavior.

### 5.1 The Challenge: Simulating Concurrent Hardware

Real hardware is truly parallel - millions of gates computing simultaneously, flip-flops clocking independently, signals propagating through wires. A simulator runs on a sequential CPU - one instruction at a time. How do we create the illusion of parallelism?

The answer: **Event-driven simulation with cooperative multitasking**.

**Event-Driven:** Don't simulate every nanosecond. Jump from interesting event to interesting event. If nothing happens between 10ns and 100ns, don't simulate that interval - jump directly from 10ns to 100ns.

**Cooperative Multitasking:** Processes (coroutines) voluntarily yield control at `wait()` calls. The kernel decides which process runs next based on which events have fired. No preemption, no threads - pure cooperation.

This is exactly how SystemC works, and it's proven effective for hardware simulation.

### 5.2 Event Queue: The Timeline of the Simulation

The event queue is the fundamental data structure. It's a priority queue ordered by time - earliest events first.

**What's in the Queue?**

Each entry is a pair: `(time, event)`

```
Event Queue at start:
  (10ns, clk.posedge)
  (20ns, clk.negedge)
  (30ns, clk.posedge)
  (40ns, clk.negedge)
  ...
```

The clock generator pre-schedules its edges. Other events get added dynamically as simulation runs.

**Priority Queue Operations:**

- **Insert (push):** Add event at specified time, maintaining time order. O(log n) with binary heap.
- **Extract (pop):** Remove and return earliest event. O(log n).
- **Peek:** Look at earliest without removing. O(1).

Why these operations? Simulation constantly pops the next event, processes it, and maybe inserts new events.

**Handling Simultaneous Events:**

Multiple events might be scheduled for the same time. They're ordered by a secondary key: **delta cycle number**.

```
(10ns, delta=0, clk.posedge)
(10ns, delta=1, data.value_change)
(10ns, delta=2, result.value_change)
(15ns, delta=0, clk.negedge)
```

Delta cycles provide sub-ordering at the same simulation time. More on this shortly.

**Binary Heap Implementation:**

We use a binary heap because:
- Efficient insert/extract: O(log n)
- Simple to implement
- Good cache locality (array-based)
- No rebalancing needed (unlike trees)

Alternative (skip list, Fibonacci heap) don't provide enough benefit to justify complexity.

### 5.3 Delta Cycles: Zero-Time Ordering

Delta cycles are the most subtle and important concept in event-driven simulation. They solve the "simultaneous event" problem.

**The Problem:**

At time 10ns, the clock edges. This triggers Process A, which writes signal X. Writing X triggers Process B, which writes signal Y. All of this happens "at 10ns" but needs ordering.

If we naively ran Process A and immediately updated X, then Process A would see the new value of X if it reads it again. That's wrong - within a single evaluation, processes should see a consistent snapshot of state.

**The Solution: Deferred Updates**

Signal writes don't take effect immediately. They're deferred to the next **delta cycle**:

```
Time 10ns:
  Delta 0 (Evaluation):
    - Clock edge fires
    - Process A wakes, runs
    - Process A writes X (write is SCHEDULED, not immediate)
    
  Delta 1 (Update):
    - All scheduled writes commit (X updates)
    - value_change events fire
    
  Delta 1 (Evaluation):
    - X's value_change wakes Process B
    - Process B runs, writes Y (SCHEDULED)
    
  Delta 2 (Update):
    - Y updates
    
  Delta 2 (Evaluation):
    - No processes wake up
    - STABLE, advance time
```

Each delta is a complete evaluate-update cycle. We iterate until reaching stability (no new updates).

**Why This Works:**

Within delta 0 evaluation, both Process A and any other processes running see the **same** state - X has not changed yet. This determinism is crucial.

The update phase commits all writes simultaneously (conceptually). Then evaluation sees the new state. This models hardware's propagation delays without simulating real time.

**Delta Cycle Limit:**

We set a maximum (e.g., 1000 deltas) to detect infinite loops:

```cxy
// Bad design: combinational loop
Process A:
    if (x == 0) y << 1
    if (x == 1) y << 0

Process B:
    if (y == 0) x << 1
    if (y == 1) x << 0
```

This creates infinite oscillation. After 1000 deltas, we raise an error: "Delta cycle overflow - possible combinational loop".

**Comparison to Real Hardware:**

Real hardware has propagation delays measured in picoseconds. Delta cycles model these delays conceptually but with zero simulation time. It's an abstraction that provides ordering without explicit delay modeling.

### 5.4 Coroutine Suspend/Resume: Process Management

Processes are coroutines. Understanding how they suspend and resume is key to the kernel's operation.

**Process State Diagram:**

```
    [Start]
       ↓
   async launch
       ↓
   [Running] ──────────────────┐
       ↓                       │
   wait(event)                 │ (loops back)
       ↓                       │
   suspend()                   │
       ↓                       │
  [Suspended] ─────────────────┤
       ·                       │
       · (waiting...)          │
       ·                       │
   event fires                 │
       ↓                       │
   resume()                    │
       ↓                       │
   [Running] ───────────────────┘
```

**The Wait Map:**

The kernel maintains a map: `Event → List[Coroutine]`

When a process calls `wait(event)`:
1. Get current coroutine via `running()`
2. Add coroutine to event's wait list: `_wait_map[event].push(coro)`
3. Suspend: `coro.suspend()`

When an event fires:
1. Look up coroutines waiting on it: `waiters = _wait_map[event]`
2. Resume each: `for coro in waiters: coro.resume()`
3. Clear the list: `_wait_map.remove(event)`

**Why This Works:**

Cxy's symmetric coroutines give us the primitives we need:
- `running()` - get current coroutine handle
- `coro.suspend()` - yield control
- `coro.resume()` - wake up suspended coroutine

The kernel holds coroutine handles and orchestrates their execution. No OS threads, no context switching overhead - just lightweight coroutines.

**Multiple Waiters:**

Many processes might wait on the same event (e.g., multiple flip-flops on same clock edge). The wait list holds all of them. When the event fires, all resume.

Resume order doesn't matter - they all run to completion (or next wait) before the update phase. Their relative order doesn't affect semantics due to deferred updates.

**One-Shot vs Persistent:**

Events are one-shot: fire, resume waiters, clear list. Next time, processes must wait again. This matches the wrapper pattern:

```cxy
while (true) {
    wait(event)     // Re-register every time
    process()
}
```

### 5.5 The Main Simulation Loop: Putting It All Together

Now we understand all the pieces. Let's see how they fit together in the main simulation loop.

**Algorithm (Pseudocode):**

```
func run(duration: Time) {
    end_time = current_time + duration
    
    while (!event_queue.empty() && current_time < end_time) {
        // 1. Get next timed event
        (time, event) = event_queue.pop()
        
        // 2. Advance time
        current_time = time
        
        // 3. Add to delta cycle queue
        delta_events.push(event)
        
        // 4. Run delta cycles until stable
        delta_number = 0
        loop {
            // Evaluation phase
            evaluate_delta(delta_events)
            delta_events.clear()
            
            // Update phase
            update_signals()
            
            // Check stability
            if delta_events.empty():
                break  // Stable, done with this time point
            
            delta_number += 1
            if delta_number > 1000:
                error("Delta cycle overflow")
        }
    }
}
```

**Evaluation Phase:**

```
func evaluate_delta(events):
    for event in events:
        waiters = wait_map.get(event)
        for coro in waiters:
            coro.resume()           // Process runs until next wait()
        wait_map.remove(event)
```

Processes run until they call wait() again, which suspends them and adds them to a (possibly different) event's wait list.

**Update Phase:**

```
func update_signals():
    for signal in dirty_signals:
        signal.commit_write()       // _current = _next
        if signal.changed():
            delta_events.push(signal.value_change)
    dirty_signals.clear()
```

Signal writes that were scheduled during evaluation now commit. If values actually changed, schedule value_change events for the next delta.

**Why This Order?**

Evaluation → Update → Check is crucial:

1. **Evaluation:** Processes run with current state snapshot
2. **Update:** All writes commit simultaneously
3. **Check:** Did updates create new events? If yes, repeat

This guarantees all processes in a delta see the same state (before updates) and all updates happen "simultaneously" (between deltas).

### 5.6 Time Advancement: Jumping Through Time

Unlike cycle-accurate simulators that step through every clock cycle, event-driven simulation **jumps** from event to event.

**Sparse Time:**

```
Simulation timeline:
0ns: Initial event
10ns: Clock edge
10ns: (delta cycles)
25ns: Timeout event
25ns: (delta cycles)
100ns: Clock edge
...
```

Between 10ns and 25ns, nothing happens - no events, no computation. The simulator jumps directly from 10ns to 25ns. This is why event-driven simulation is fast for loosely-timed models.

**Calculating Next Event Time:**

After finishing delta cycles at current time, peek at the event queue:

```
next_event = event_queue.peek()
if next_event:
    next_time = next_event.time
    jump to next_time
else:
    simulation complete (no more events)
```

Time advances in discrete jumps, not continuous flow.

**Time Limits:**

The `run(duration)` function sets a time limit. Events beyond that limit are not processed (though they remain in the queue for future runs).

This enables incremental simulation:
```cxy
ctx.run(100.ns)     // Simulate 0-100ns
// Check results, modify signals
ctx.run(100.ns)     // Simulate 100-200ns
```

### 5.7 Signal Update Integration

Signals integrate with the kernel through the dirty signal list.

**When Signal is Written:**

```cxy
// User code
data << 42

// Inside Signal.write()
func write(value: T) {
    _next = value
    _changed = true
    SimContext.current().register_dirty_signal(this)
    SimContext.current().schedule_delta(value_change)
}
```

Writing a signal:
1. Stores pending value
2. Marks signal dirty
3. Registers with kernel for update phase
4. Schedules value_change event for next delta

**Update Phase:**

```cxy
func update_signals() {
    for signal in dirty_signals {
        old = signal._current
        new = signal._next
        
        signal._current = new
        signal._next = null
        signal._changed = false
        
        if old != new {
            signal.value_change._fire()
        }
    }
    dirty_signals.clear()
}
```

All dirty signals update "simultaneously". Only actually-changed signals fire events.

**Why Dirty Tracking?**

We could update all signals every delta. But that's wasteful - most signals don't change. Dirty tracking ensures we only update what's necessary.

### 5.8 Clock Generator Integration

Clocks are special - they generate events automatically. How do they integrate with the kernel?

**Clock Generator Coroutine:**

```cxy
// Inside Clock
async _generate() {
    while (_running) {
        // High phase
        _bit.write_immediate(One)
        _schedule_self(high_time)
        suspend()
        
        // Low phase
        _bit.write_immediate(Zero)
        _schedule_self(low_time)
        suspend()
    }
}

func _schedule_self(delay: Time) {
    var wake_event = Event()
    SimContext.schedule(wake_event, delay)
    wait(wake_event)
}
```

The clock generator is itself a coroutine that schedules wake-up events for itself.

**Pre-populating the Queue:**

When clock starts, it schedules its first edge. That edge schedules the next edge, and so on. The queue fills with future clock events.

This is why clocks need explicit `start()` - until started, they don't schedule events, so they don't toggle.

**Edge Event Generation:**

When clock's bit changes, edge detection fires posedge/negedge events. These wake processes waiting on clock edges.

The clock doesn't know who's listening - it just generates edges. The kernel delivers events to waiting processes.

### 5.9 Complete Execution Trace

Let's trace our demo.cxy example through the simulation kernel to see all the pieces working together.

**Setup (Time 0ns, Initialization):**

```
1. Testbench creates:
   - SimContext
   - Clock("clk", 10.ns)
   - Signals for reset, count
   
2. Clock.start() is called:
   - Launches async _generate() coroutine
   - _generate schedules wake event at 5ns (half period for first edge)
   - Suspends
   
Event queue: [(5ns, clk_wake_event)]

3. Counter constructor runs:
   - Binds ports
   - Launches async _process_wrapper()
   - _process_wrapper calls wait(clk.posedge())
   - Coroutine suspends, added to clk.posedge wait list
   
Wait map: {clk.posedge → [counter._process_wrapper]}
```

**Time Advances to 5ns:**

```
Event queue pop: (5ns, clk_wake_event)
Current time = 5ns

Delta 0 Evaluation:
  - clk_wake_event fires
  - Clock._generate resumes
  - Writes clk bit = 1 (rising edge)
  - This is immediate write for clocks
  - Edge detected: posedge event scheduled for delta 1
  - Clock schedules next wake at 10ns
  - Clock suspends
  
Delta 0 Update:
  - No signal writes to commit (clock used immediate write)
  
Delta 1 Evaluation:
  - clk.posedge event fires
  - Counter._process_wrapper resumes (from wait list)
  - Calls process()
  - process() reads reset, computes, writes count << value
  - write schedules count update for next delta
  - Returns to wrapper
  - Calls wait(clk.posedge()) again
  - Suspends
  
Delta 1 Update:
  - count signal updates (commit pending write)
  - count.value_change fires (no one waiting on it)
  
Delta 2 Evaluation:
  - No events
  - STABLE
  
Event queue: [(10ns, clk_wake_event)]
Wait map: {clk.posedge → [counter._process_wrapper]}
```

**Time Advances to 10ns:**

```
(Same pattern repeats: clock wakes, toggles low, posedge doesn't fire)
```

**Time Advances to 15ns:**

```
(Clock wakes, toggles high again, posedge fires, counter runs...)
```

This cycle continues throughout simulation. The kernel orchestrates all the pieces automatically.

---

## 6. Implementation Plan

Now that we understand what we're building and why, let's plan the implementation. Breaking the project into phases with clear dependencies ensures steady progress and early validation.

### 6.1 Phased Approach Rationale

Why phases instead of building everything at once?

**Early Testing:** Get something running quickly to validate core assumptions. Better to discover fundamental issues early than after months of work.

**Clear Dependencies:** Some parts depend on others. Implement foundations before building on them.

**Incremental Complexity:** Start simple, add features progressively. This keeps the codebase working at each step.

**Learning Curve:** Each phase teaches us about the domain. Later phases benefit from earlier experience.

**Motivation:** Seeing tangible progress (a working, if limited, system) maintains momentum.

### 6.2 Phase 1: Core Types and Basic Signals

**Goal:** Implement fundamental types that everything else builds on. Get signals working without simulation.

**Deliverables:**
- Time type with arithmetic and comparison
- Event type with wait lists
- Signal[T] with read/write (no kernel yet)
- Bit with four-state logic
- Basic unit tests for each type

**Why Start Here:**

These types are prerequisites for everything else. We can test them independently without the complexity of simulation. If Signal[T] doesn't work correctly, nothing built on it will work.

**Implementation Tasks:**

1. **Time:**
   - Define TimeUnit enum
   - Implement Time struct with value + unit
   - Arithmetic operators (+, -, *, /)
   - Comparison operators (==, <, >, etc.)
   - Conversion methods (toNanoseconds, etc.)
   - Literal syntax support (10.ns, 5.us)

2. **Event:**
   - Basic event struct
   - Wait list (List[Coroutine])
   - notify() methods (immediate and delayed)
   - No actual firing yet (needs kernel)

3. **Signal[T]:**
   - Generic struct over type T
   - _current and _next value storage
   - read() returns _current
   - write() stores in _next, sets dirty flag
   - Mock update() for testing (actually commit _next → _current)

4. **Bit:**
   - Character/integer literal support ('0', '1', 'X', 'Z' and 0, 1)
   - Wraps Signal[wchar] internally
   - Logical operators (&, |, ^, ~) via operator overloading
   - Edge detection placeholders (events exist, but won't fire yet)
   - Overloaded constructors and comparison operators

**Test Strategy:**

Write unit tests for each type:
```cxy
test "Time arithmetic" {
    var t1 = 10.ns
    var t2 = 5.ns
    assert t1 + t2 == 15.ns
    assert t1 > t2
}

test "Signal read/write" {
    var sig = Signal[u32]("test", 0)
    assert sig.read() == 0
    sig.write(42)
    sig._update()  // Mock update
    assert sig.read() == 42
}

test "Bit logic with character literals" {
    var a = Bit("a", '1')
    var b = Bit("b", '0')
    assert (a & b).read() == '0'
}

test "Bit logic with integer literals" {
    var a = Bit("a", 1)
    var b = Bit("b", 0)
    assert (a & b).read() == '0'
    assert a.read() == 1  // Can compare with integer
}

test "Bit unknown states" {
    var x = Bit("x", 'X')
    var z = Bit("z", 'Z')
    assert x.read() == 'X'
    assert z.read() == 'Z'
}
```

**Validation:** All core type tests pass. Signal read/write works. Time arithmetic correct.

### 6.3 Phase 2: Simulation Kernel Foundation

**Goal:** Implement the simulation kernel without plugins. Enable manual component creation and simulation.

**Deliverables:**
- SimContext with event queue
- Delta cycle manager
- Coroutine wait/resume logic
- Clock generator
- Working simulation loop

**Why Now:**

With core types working, we can build the kernel that orchestrates them. This phase validates the simulation algorithm before adding plugin complexity.

**Implementation Tasks:**

1. **Event Queue:**
   - Priority queue implementation (binary heap)
   - TimedEvent struct with time + delta + event
   - push/pop/peek operations
   - Tests for ordering

2. **Delta Cycle Manager:**
   - Delta event queue (regular queue)
   - Dirty signal tracking
   - processDeltaCycle() method
   - Delta limit detection

3. **Coroutine Manager:**
   - Wait map: Event → List[Coroutine]
   - registerWait() method
   - fireEvent() method  
   - Integration with running()

4. **SimContext:**
   - Integrate all managers
   - run(duration) method
   - Main simulation loop
   - wait() function using running()

5. **Clock:**
   - Auto-generation coroutine
   - Start/stop methods
   - Period and duty cycle support
   - Integration with SimContext scheduling

**Test Strategy:**

Write tests that manually create components (without plugin):

```cxy
test "Manual counter simulation" {
    var ctx = SimContext()
    var clk = Clock("clk", 10.ns)
    var count = Signal[u32]("count", 0)
    var value: u32 = 0
    
    // Manual process (what plugin will generate)
    async manual_process() {
        while (true) {
            wait(clk.posedge())
            value += 1
            count << value
        }
    }
    
    clk.start()
    async manual_process()
    
    ctx.run(50.ns)
    assert count.read() == 5  // 5 clock edges
}
```

This validates the kernel works before worrying about generated code.

**Validation:** Can run simple simulations with manual process creation. Delta cycles work. Time advances correctly.

### 6.4 Phase 3: Port Types and Binding

**Goal:** Implement port wrappers and binding semantics. Enable component-signal connections.

**Deliverables:**
- InPort[T], OutPort[T], InOutPort[T] types
- Binding methods
- Direction enforcement (compile-time and runtime)
- Test components with manual ports

**Why Now:**

Before the plugin can generate components with ports, the port types must exist and work. This phase provides the infrastructure the plugin will use.

**Implementation Tasks:**

1. **InPort[T]:**
   - Optional reference to Signal[T]
   - bind() method
   - read() method (delegates to signal)
   - valueChange() event access
   - Compile-time: no write() method

2. **OutPort[T]:**
   - Optional reference to Signal[T]
   - bind() method
   - write() method (delegates to signal)
   - Compile-time: no read() method

3. **InOutPort[T]:**
   - Both read() and write() methods

4. **Validation:**
   - Runtime checks that ports are bound before use
   - Clear error messages if unbound

**Test Strategy:**

```cxy
test "Port binding" {
    struct ManualComponent {
        input: InPort[u32]
        output: OutPort[u32]
        
        func init(inp: &Signal[u32], out: &Signal[u32]) {
            input.bind(inp)
            output.bind(out)
        }
        
        func transfer() {
            output << input.read()
        }
    }
    
    var inp = Signal[u32]("inp", 42)
    var out = Signal[u32]("out", 0)
    var comp = ManualComponent(&inp, &out)
    
    comp.transfer()
    out._update()
    assert out.read() == 42
}
```

**Validation:** Ports bind correctly. Read/write delegate to signals. Direction enforced.

### 6.5 Phase 4: Plugin Implementation

**Goal:** Implement the plugin that transforms attributed code. This is the largest and most complex phase.

**Deliverables:**
- Plugin skeleton and registration
- @rtl::component transformation
- @in/@out/@inout transformation
- @method transformation
- @rtl::testbench transformation
- Comprehensive test suite

**Why Now:**

All the runtime pieces exist. The plugin needs to generate code using those pieces. With phases 1-3 complete, we know what code to generate and can test it immediately.

**Implementation Tasks:**

1. **Plugin Infrastructure:**
   - Register plugin with Cxy compiler
   - AST visitor pattern setup
   - Symbol table for discovered items
   - Error reporting framework

2. **Pass 1: Discovery:**
   - Find @rtl::component structs
   - Find @in/@out/@inout fields
   - Find @method functions
   - Build symbol table

3. **Pass 2: Validation:**
   - Validate attribute contexts
   - Validate port types
   - Validate trigger references
   - Report all errors clearly

4. **Pass 3: Type Transformation:**
   - Transform port field types
   - Track original types for constructor

5. **Pass 4: Constructor Generation:**
   - Build parameter list from ports
   - Generate binding statements
   - Generate process launch statements
   - Create AST node for constructor

6. **Pass 5: Process Wrapper Generation:**
   - Parse trigger specifications
   - Generate async wrapper for each @method
   - Handle single and multiple triggers
   - Create AST nodes for wrappers

7. **Pass 6: Testbench Transformation:**
   - Inject SimContext creation
   - Auto-start clocks
   - Transform simulate() calls
   - Add cleanup code

8. **Pass 7: Code Injection:**
   - Insert generated methods into structs
   - Wrap test bodies
   - Preserve source locations

**Test Strategy:**

Create test cases for each transformation:

```cxy
// Test basic component generation
@rtl::component
struct TestComponent {
    @in clk: Clock
    @out data: u32
    
    @method(clk: "posedge")
    func process() {
        data << 42
    }
}

// Verify generated code works
test "Component instantiation" {
    var ctx = SimContext()
    var clk = Clock("clk", 10.ns)
    var data = Signal[u32]("data", 0)
    
    var comp = TestComponent(&clk, &data)  // Generated constructor
    clk.start()
    ctx.run(10.ns)
    assert data.read() == 42
}
```

Test error cases:

```cxy
// Should fail: invalid port type
@rtl::component
struct Bad {
    @in data: InvalidType  // Error expected
}

// Should fail: undefined trigger
@rtl::component
struct Bad2 {
    @method(missing: "posedge")  // Error expected
    func process() {}
}
```

**Validation:** Plugin transforms code correctly. Generated code compiles. Generated code runs. Errors are caught and reported clearly.

### 6.6 Phase 5: Advanced Features and Optimization

**Goal:** Add missing features, optimize performance, improve usability.

**Deliverables:**
- Bus[N] type
- waitAny() for multiple triggers
- Waveform tracing (VCD output)
- Performance optimization
- Documentation and examples

**Why Last:**

These are enhancements to a working system. Better to have a complete but basic system than an incomplete feature-rich one.

**Implementation Tasks:**

1. **Bus[N]:**
   - Fixed-width bit vector type
   - Bit slicing operations
   - Arithmetic and bitwise operators
   - Four-state logic propagation

2. **waitAny():**
   - Library function for OR sensitivity
   - Register with multiple events
   - Resume on first fire
   - Unregister from others

3. **Waveform Tracing:**
   - VCD file format support
   - Trace signal value changes
   - Configurable trace scope
   - @rtl::testbench(trace="file.vcd") option

4. **Performance:**
   - Profile hot paths (signal update, event firing)
   - Optimize data structures (better heap? hash map?)
   - Minimize allocations in hot paths
   - Consider parallel evaluation (multiple processes on multi-core)

5. **Documentation:**
   - User guide with examples
   - API reference
   - Plugin attribute reference
   - Performance tuning guide

6. **Examples:**
   - Basic counter (done in demo.cxy)
   - FIFO buffer
   - Simple CPU model
   - Communication protocol (UART, SPI)

**Validation:** All features work. Performance acceptable for reasonable model sizes. Documentation complete.

### 6.7 Testing Strategy

Testing happens at multiple levels throughout all phases:

**Unit Tests:** Test individual types in isolation. Fast, focused.

**Integration Tests:** Test components working together. Validate kernel + signals + ports.

**Plugin Tests:** Test code generation. Compare generated AST to expected.

**End-to-End Tests:** Full simulations of example designs. Validate entire system.

**Error Tests:** Verify that invalid code produces clear error messages.

**Performance Tests:** Benchmark simulation speed. Track regressions.

Each phase adds tests. By Phase 5, we have comprehensive coverage.

### 6.8 Dependencies Between Phases

```
Phase 1: Core Types
    ↓
Phase 2: Simulation Kernel ← depends on Phase 1
    ↓
Phase 3: Port Types ← depends on Phase 1, 2
    ↓
Phase 4: Plugin ← depends on Phase 1, 2, 3
    ↓
Phase 5: Advanced Features ← depends on all previous
```

This dependency structure means:
- Can't start Phase 2 until Phase 1 complete
- Can start Phase 3 as soon as Phase 2 works
- Phase 4 needs everything before it
- Phase 5 is pure enhancement

Phases 1-4 are the "minimum viable product" - a working RTL library with plugin support.

### 6.9 Risk Mitigation

**Risk:** Cxy's coroutine model doesn't work as expected for our use case.
**Mitigation:** Phase 2 validates this early. If it doesn't work, we discover it before investing in the plugin.

**Risk:** Plugin API is insufficient or too complex.
**Mitigation:** Phase 4 is isolated. If plugin development stalls, we still have phases 1-3 (manual component creation works).

**Risk:** Performance is inadequate for real use.
**Mitigation:** Profile in Phase 2 with simple models. Optimize hot paths before complexity grows. Phase 5 dedicates time to optimization.

**Risk:** Generated code is hard to debug.
**Mitigation:** Preserve source locations in AST. Keep generated code readable (meaningful names, comments). Test thoroughly.

### 6.10 Success Criteria

How do we know we're done?

**Phase 1:** All core type unit tests pass.

**Phase 2:** Can run multi-process simulations manually. Demo.cxy logic works without plugin.

**Phase 3:** Ports bind correctly. Direction enforced.

**Phase 4:** Plugin generates correct code. Demo.cxy works as written (with attributes). Error messages are clear.

**Phase 5:** Advanced features work. Performance acceptable (>1M signal updates/second). Documentation complete.

**Overall:** The demo.cxy example runs, produces correct results, and serves as template for new designs.

### 6.11 Estimated Timeline

This is order-of-magnitude estimation, not a commitment:

- **Phase 1:** 1-2 weeks (foundational types, relatively straightforward)
- **Phase 2:** 2-3 weeks (simulation kernel is complex, needs careful testing)
- **Phase 3:** 1 week (ports are wrappers, relatively simple)
- **Phase 4:** 3-4 weeks (plugin is most complex, lots of edge cases)
- **Phase 5:** 2-3 weeks (enhancement and polish)

**Total:** 9-13 weeks for a complete, working RTL library with plugin support.

This assumes one developer working full-time. Multiple developers can parallelize some work (e.g., Phase 3 while Phase 2 finalizes).

---

## Conclusion

We've designed a system-level modeling library for Cxy that combines:

- **SystemC's proven semantics:** Event-driven simulation, delta cycles, ports and signals
- **Cxy's unique strengths:** Native coroutines for processes, powerful plugin for code generation, strong type system for safety
- **Clean user experience:** Declarative attributes, minimal boilerplate, clear error messages

The design leverages Cxy's features to create a library that's more natural to use than SystemC while maintaining compatibility with its well-understood simulation model.

The phased implementation plan provides a clear path from core types to a complete, working system. Each phase builds on the previous, with early validation of critical assumptions.

This design document serves as the blueprint for implementation. It explains not just what to build, but why design decisions were made, how the pieces fit together, and what success looks like.

---

**Appendix: Quick Reference**

**Key Types:**
- `Time` - Simulation time with multiple units
- `Event` - Synchronization primitive
- `Signal[T]` - Generic time-varying signal
- `Bit` - Four-state logic with edge detection
- `Clock` - Auto-generating periodic signal
- `InPort[T]/OutPort[T]` - Direction-enforced port wrappers
- `SimContext` - Simulation kernel

**Key Plugin Actions:**
- `@rtl::component` - Transform struct into component
- `@in/@out/@inout` - Mark fields as ports
- `@method(trigger)` - Transform method into process
- `@rtl::testbench` - Wrap test with simulation setup

**Simulation Loop:**
1. Pop next event from queue
2. Advance time
3. Run delta cycles (evaluate → update → repeat until stable)
4. Continue to next event

**Delta Cycle:**
- **Evaluation:** Resume waiting processes, they run and schedule writes
- **Update:** Commit all pending writes, fire value_change events
- **Check:** New events? If yes, repeat

This design enables clean, type-safe hardware modeling in Cxy.

---
