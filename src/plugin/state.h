//
// RTL Plugin - State and data structures
//
// This plugin processes @rtl::component attributes and generates:
// - Port type transformations (@in T → InPort[T])
// - Constructor with port binding
// - Async process wrappers for @method functions
//

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <cxy/core/htable.h>
#include <cxy/core/mempool.h>
#include <cxy/core/strpool.h>
#include <cxy/core/log.h>
#include <cxy/ast.h>
#include <cxy/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Interned attribute names (defined in rtl-plugin.c)
// ============================================================================

extern cstring rtl_input;
extern cstring rtl_output;
extern cstring rtl_inout;
extern cstring rtl_signal_attr;
extern cstring rtl_method;

// Interned type names
extern cstring rtl_clock;
extern cstring rtl_signal;
extern cstring rtl_bitvector;
extern cstring rtl_bitsignal;

// Interned wrapper names
extern cstring rtl_inport;
extern cstring rtl_outport;
extern cstring rtl_inoutport;
extern cstring rtl_inbitport;
extern cstring rtl_outbitport;
extern cstring rtl_inoutbitport;

// Interned event names
extern cstring rtl_posedge;
extern cstring rtl_negedge;
extern cstring rtl_valueChanged;
extern cstring rtl_change;

// Interned method names
extern cstring rtl_running;
extern cstring rtl_wait;
extern cstring rtl_waitAny;
extern cstring rtl_waitAll;

// Interned trigger keywords
extern cstring rtl_and;

// Interned type names for EventGroup
extern cstring rtl_EventGroup;

// ============================================================================
// Port direction
// ============================================================================

typedef enum {
    rtlPortNone = 0,
    rtlPortIn,
    rtlPortOut,
    rtlPortInOut
} RtlPortDirection;

// ============================================================================
// Port information (discovered during AST scan)
// ============================================================================

typedef struct RtlPortInfo {
    const char        *name;          // Field name (interned)
    RtlPortDirection   direction;     // @in, @out, or @inout
    AstNode           *fieldNode;     // Mutable field AST node (structField)
    AstNode           *paramType;     // Constructor parameter type (for non-clock: Signal[T] or BitSignal[N])
    AstNode           *wrapperArgs;   // Generic args for port wrapper (e.g., [N] for InBitPort[N])
    bool               isClock;       // True if type is &Clock
    struct RtlPortInfo *next;
} RtlPortInfo;

// ============================================================================
// Signal information (internal signals - NOT constructor parameters)
// ============================================================================

typedef struct RtlSignalInfo {
    const char         *name;         // Field name (interned)
    AstNode            *fieldNode;    // Mutable field AST node (structField)
    AstNode            *defaultValue; // Default value expression (required)
    struct RtlSignalInfo *next;
} RtlSignalInfo;

// ============================================================================
// Method trigger specification
// ============================================================================

// Trigger event - single port+event pair
typedef struct RtlTriggerEvent {
    const char *portName;     // Port to wait on (e.g., "clk", "rst")
    const char *eventName;    // Event name (e.g., "posedge", "valueChanged")
    struct RtlTriggerEvent *next;
} RtlTriggerEvent;

// Trigger mode - determines wait semantics
typedef enum {
    rtlTriggerSingle,       // Single event (backward compat)
    rtlTriggerAny,          // Multiple events, OR/waitAny semantics
    rtlTriggerAll           // Multiple events, AND/waitAll semantics
} RtlTriggerMode;

// Method trigger specification (supports single or multiple events)
typedef struct RtlMethodTrigger {
    RtlTriggerMode mode;      // SINGLE, ANY, or ALL
    RtlTriggerEvent *events;  // Linked list of trigger events
    int eventCount;           // Number of events
} RtlMethodTrigger;

// ============================================================================
// Method information (discovered during AST scan)
// ============================================================================

typedef struct RtlMethodInfo {
    const char           *name;      // Method name (interned)
    AstNode              *funcNode;  // Mutable function AST node
    RtlMethodTrigger      trigger;   // Parsed from @method(port: "event")
    struct RtlMethodInfo *next;
} RtlMethodInfo;

// ============================================================================
// Component information (one per @rtl::component struct)
// ============================================================================

typedef struct RtlComponentInfo {
    const char            *name;        // Struct/class name (interned)
    AstNode               *structNode;  // Mutable struct/class AST node
    RtlPortInfo           *ports;       // Linked list of ports
    RtlSignalInfo         *signals;     // Linked list of internal signals
    RtlMethodInfo         *methods;     // Linked list of @method functions
    AstNode               *generatedWrappers;  // Generated wrapper functions
    int                    portCount;    // Number of ports
    int                    signalCount;  // Number of internal signals
    int                    methodCount;  // Number of methods
    struct RtlComponentInfo *next;
} RtlComponentInfo;

// ============================================================================
// Plugin state (registered via cxyPluginInitialize)
// ============================================================================

typedef struct {
    RtlComponentInfo *components;    // Linked list of discovered components
    MemPool          *pool;          // Reference to CXY's memory pool
    StrPool          *strings;       // Reference to CXY's string pool
    struct Log       *L;             // Reference to CXY's logger
} RtlPluginState;

// ============================================================================
// Function declarations (implemented in separate .c files)
// ============================================================================

// Discovery pass - scan AST to find ports and methods
RtlComponentInfo *rtlDiscoverComponent(CxyPluginContext *ctx,
                                        AstNode *classNode,
                                        Log *L,
                                        const FileLoc *loc);

// Validation pass - validate port types, triggers, events
bool rtlValidateComponent(Log *L, RtlComponentInfo *comp);

// Transformation pass - wrap port types (T → InPort[T]/OutPort[T]/InOutPort[T])
void rtlTransformComponent(MemPool *pool, RtlComponentInfo *comp, StrPool *strings);

// Wrapper generation - create async wrapper functions for @method
void rtlGenerateWrappers(MemPool *pool, StrPool *strings, RtlComponentInfo *comp);

// Constructor generation - create `init` constructor and inject it
void rtlGenerateConstructor(MemPool *pool, StrPool *strings, RtlComponentInfo *comp);

#ifdef __cplusplus
}
#endif
