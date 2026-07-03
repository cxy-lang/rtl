# rtl - System-Level Hardware Modeling for Cxy

A modern, type-safe system-level hardware modeling library for Cxy, inspired by SystemC but designed to leverage Cxy's native coroutines, compile-time plugins, and strong type system.

**Version:** 0.1.0  
**License:** MIT  
**Author:** Carter Mbotho

## Overview

The `rtl` library enables hardware designers to model digital systems at a behavioral level using event-driven simulation. It provides the core primitives for describing concurrent hardware components, their communication through signals, and timing relationships—all with compile-time safety guarantees and clean, declarative syntax.

### Key Features

- **Native Coroutine-Based Processes** - Hardware behaviors map naturally to Cxy's symmetric coroutines
- **Compile-Time Component Generation** - Plugin system transforms declarative annotations into efficient simulation code
- **Type-Safe Signal Communication** - Generic `Signal[T]` types prevent connection mismatches
- **Four-State Logic** - `Bit` type supports `'0'`, `'1'`, `'X'` (unknown), and `'Z'` (high-impedance) states
- **BitVector with Slicing** - Fixed-width bit vectors with zero-copy slice views and full four-state support
- **BitSignal with Slice Propagation** - Signals that automatically propagate changes between parent and slice views
- **Delta-Cycle Simulation Kernel** - Deterministic, event-driven execution matching hardware semantics
- **Clock & Event Management** - Automatic periodic signal generation and synchronization primitives
- **EventGroup** - Multi-event synchronization with ANY (OR) and ALL (AND/barrier) semantics
- **VCD Waveform Tracing** - Export simulation results for visualization in waveform viewers
- **Port Direction Enforcement** - `InPort`, `OutPort`, and `InOutPort` types enforce correct signal flow at compile time

## Installation

Add `rtl` to your Cxy project:

```bash
cxy package add rtl
```

The package includes a required plugin that enables the `@rtl::component` and related attributes.

## Quick Start

Here's a simple counter component that increments on each clock edge:

```cxy
import { Clock, ns, Signal, Bit, simContext } from "@rtl"
import plugin "rtl" as rtl

@rtl::component
class Counter {
    @input clk: Clock
    @input reset: Bit
    @output count: u32

    _value: u32 = 0

    @method(clk: "posedge")
    func increment() {
        if *reset == Bit('1') {
            _value = 0
        } else {
            _value += 1
        }
        count << _value
    }
}

func main() {
    var ctx = simContext()
    var clk = Clock("clk", 10.ns)  // 10ns period clock
    var reset = Signal[Bit](Bit('1'))
    var count = Signal[u32](0)
    
    clk.start()
    var counter = Counter(&clk, reset, count)
    
    // Reset phase
    ctx.run(20.ns)
    
    // Release reset and count
    reset << Bit('0')
    ctx.run(100.ns)
    
    println("Final count: ", count.read())
}
```

## Core Concepts

### Time Representation

The `Time` type represents simulation time with natural syntax using literal suffixes:

```cxy
import { ps, ns, us, ms, sec } from "@rtl"

var delay = 10.ns      // 10 nanoseconds
var timeout = 5.us     // 5 microseconds
var period = 100.ps    // 100 picoseconds

// Time arithmetic
var total = delay + timeout
var scaled = delay * 2
```

### Signals and Ports

Signals are the primary communication mechanism between hardware components:

```cxy
import { Signal } from "@rtl"

var dataSignal = Signal[u32](0)    // 32-bit signal initialized to 0
dataSignal << 42                    // Write value to signal
var value = dataSignal.read()      // Read current value
```

Ports provide typed, directional interfaces to components:

- `InPort[T]` - Read-only input (can't be written inside component)
- `OutPort[T]` - Write-only output (can't be read inside component)
- `InOutPort[T]` - Bidirectional port (can read and write)

### Four-State Bit Logic

The `Bit` type models digital logic values including unknown and high-impedance states:

```cxy
import { Bit, Bit0, Bit1, BitX, BitZ } from "@rtl"

var low = Bit('0')     // Logic low
var high = Bit('1')    // Logic high
var unknown = Bit('X') // Unknown/uninitialized
var highz = Bit('Z')   // High-impedance (tri-state)

// Bitwise operations handle all four states correctly
var result = low & high         // Bit('0')
var or_result = low | high      // Bit('1')
var xor_result = low ^ high     // Bit('1')
var not_result = ~high          // Bit('0')
```

### BitVector - Multi-Bit Values

`BitVector[N]` represents fixed-width vectors of N bits with full four-state logic:

```cxy
import { BitVector } from "@rtl"

// Create from integer
var addr = BitVector[8](0x3F)

// Parse from strings (binary/hex, with X/Z states)
var data = BitVector[8]("0b1010_0101")
var mask = BitVector[16]("0xABCD")
var bus = BitVector[4]("0b10XZ")

// Type-safe conversion
var value = addr as u64
var signed = addr as i64

// Bitwise and arithmetic
var result = addr & mask
var sum = addr + mask

// Indexing and slicing
var bit0 = addr.[0]
addr.[7] = Bit('1')
var lowNibble = addr.slice[0, 4]()

// String conversion
var binary = addr.toBinary()
var hex = addr.toHex()
```

### BitSignal - Signals with Slice Propagation

`BitSignal[N]` provides signals with automatic slice change propagation:

```cxy
import { BitSignal, BitVector } from "@rtl"

var dataReg = BitSignal[16](0xABCD)

// Zero-copy slices
var lowByte = dataReg.slice[0, 8]()
var highByte = dataReg.slice[8, 16]()

// Parent write notifies affected slices
dataReg << BitVector[16](0x1234)

// Slice write updates parent
lowByte << BitVector[8](0xFF)  // dataReg becomes 0x12FF

// Nested slices work
var nibble = lowByte.slice[0, 4]()
nibble << BitVector[4](0xA)

// Smart change detection - only affected signals fire events
highByte << BitVector[8](0xAB)  // lowByte unchanged, no event
```

### Clock Generation

The `Clock` class automatically generates periodic signals:

```cxy
import { Clock, ns } from "@rtl"

var clk = Clock("sys_clk", 10.ns)  // 10ns period (100MHz)
clk.start()  // Begin generating clock edges
```

### Components and Processes

Use the `@rtl::component` attribute to define hardware modules. The plugin transforms annotated fields and methods:

```cxy
@rtl::component
class MyComponent {
    @input clk: Clock           // Becomes InPort[Clock]
    @input data: u32            // Becomes InPort[u32]
    @output result: u32         // Becomes OutPort[u32]
    
    _state: u32 = 0             // Private internal state
    
    @method(clk: "posedge")     // Triggered on rising clock edge
    func process() {
        _state += *data
        result << _state
    }
}
```

Supported trigger specifications:
- `"posedge"` - Rising edge
- `"negedge"` - Falling edge
- `"anyedge"` - Either edge
- Multiple events - `@method(rst, en)` waits for ANY event (OR)
- All events - `@method(_and, rst, en)` waits for ALL events (AND)
- Mixed triggers - `@method(rst, clk: "posedge")` combines events and edges

### Events and Synchronization

Use `Event` for custom synchronization between processes:

```cxy
import { Event, wait } from "@rtl"

var dataReady = Event()

async {
    // Producer process
    wait(&dataReady)
    println("Data is ready!")
}

// Signal the event
dataReady.notify()
```

### EventGroup - Multi-Event Synchronization

`EventGroup` allows waiting on multiple events with ANY or ALL semantics:

```cxy
import { EventGroup, Event } from "@rtl"

var evt1 = Event()
var evt2 = Event()
var evt3 = Event()

// ANY semantics (OR) - resume when any event fires
var anyGroup = EventGroup(evt1, evt2, evt3)
async {
    anyGroup.waitAny()
    println("At least one event fired!")
}

// ALL semantics (AND/barrier) - resume when all events fire
var allGroup = EventGroup(evt1, evt2)
async {
    allGroup.waitAll()
    println("Both events have fired!")
}

// Use in @method triggers
@rtl::component
class MultiEventComponent {
    @input rst: bool
    @input en: bool
    
    // ANY semantics - fires when rst OR en changes
    @method(rst, en)
    func onAnyChange() { }
    
    // ALL semantics - fires when BOTH rst AND en change
    @method(_and, rst, en)
    func onAllChange() { }
}
```

### Simulation Control

The `SimContext` manages simulation time and event scheduling:

```cxy
import { simContext, ns } from "@rtl"

var ctx = simContext()

// Run for specified duration
ctx.run(100.ns)

// Check current time
var t = ctx.currentTime()
```

### VCD Waveform Tracing

Export signals for viewing in waveform tools (GTKWave, etc.):

```cxy
import { Vcd } from "@rtl"
import plugin "rtl" as rtl

var clk = Clock("clk", 10.ns)
var data = Signal[u32](0)

// Create trace file
var trace = rtl::vcd("simulation.vcd", clk)

// Add signals to trace
rtl::trace(trace, data)

// Run simulation (trace is automatically recorded)
simContext().run(1000.ns)
```

## Architecture

The library is structured in three layers:

1. **Runtime Library** (`src/*.cxy`) - Core types and simulation kernel
   - Time, Bit, BitVector, BitSignal, Signal, Clock
   - Event queue and delta cycle manager
   - SimContext (simulation scheduler)
   - Slice propagation and change detection

2. **Plugin** (`src/plugin/*.c`) - Compile-time code generation
   - Transforms `@rtl::component` classes
   - Generates port bindings and process wrappers
   - Validates component structure

3. **User Code** - Clean component definitions
   - Declarative attributes (`@input`, `@output`, `@method`)
   - Natural Cxy syntax
   - Compile-time safety

## Testing

Run the test suite:

```bash
cxy package test
```

Tests cover:
- Time arithmetic and unit conversions
- Bit four-state logic operations
- BitVector operations, slicing, and string parsing
- BitSignal slice propagation and change detection
- Signal read/write and event propagation
- EventGroup ANY/ALL semantics and multi-event triggers
- Clock generation and edge detection
- Port binding and type enforcement
- Simulation kernel delta-cycle behavior

## Design Philosophy

**Familiar to SystemC Users** - Adopts proven concepts (modules, ports, signals, delta cycles) from the IEEE 1666 standard.

**Leverage Cxy's Strengths** - Native coroutines replace macros, plugins eliminate boilerplate, type system ensures safety.

**Correct by Construction** - Compile-time validation catches errors before runtime:
- Wrong port directions
- Type mismatches in connections
- Invalid trigger specifications
- Unbound ports

**Efficient Simulation** - Binary heap event queue, lightweight coroutines, minimal overhead in critical paths.

## Documentation

- [Design Document](docs/design.md) - Comprehensive design rationale, architecture, and implementation guide
- [API Reference](docs/api.md) - Detailed API documentation (coming soon)

## Comparison to SystemC

| Feature | SystemC | rtl (Cxy) |
|---------|---------|-----------|
| Process model | Macros (`SC_METHOD`, `SC_THREAD`) | Native coroutines with `@method` |
| Module definition | `SC_MODULE` macro | `@rtl::component` attribute |
| Port types | Templates (`sc_in<T>`) | Generic types (`InPort[T]`) |
| Sensitivity lists | Manual registration | Auto-generated from `@method` trigger |
| Signal updates | `sc_signal<T>` | `Signal[T]` with `<<` operator |
| Time literals | `sc_time(10, SC_NS)` | `10.ns` (natural syntax) |
| Code generation | Preprocessor macros | Compile-time AST transformation |

## Contributing

This is an early-stage project. Contributions welcome!

## Roadmap

- [x] Core time and event types
- [x] Four-state bit logic
- [x] Signal and port infrastructure
- [x] Basic simulation kernel with delta cycles
- [x] Clock generation
- [x] EventGroup for multi-event synchronization
- [x] Plugin for `@rtl::component` transformation
- [x] VCD tracing support
- [x] Basic examples and tests
- [x] BitVector with slice operations
- [x] BitSignal with slice propagation
- [ ] Advanced port protocols (TLM-style)
- [x] Hierarchical component composition
- [ ] Performance profiling and optimization
- [ ] Coverage collection
- [ ] SystemC interoperability layer

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Related Projects

- **SystemC** - IEEE 1666 standard for system-level modeling in C++
- **Verilator** - Fast cycle-accurate HDL simulator
- **Cxy Language** - Modern systems programming language with native coroutines

---

For questions, issues, or contributions, please visit the project repository.
