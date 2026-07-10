//
// RTL Plugin - Process Wrapper Code Generation
//
// Generates async wrapper functions for @method annotated functions.
// Example:
//   @method(clk: "posedge")
//   func count() { ... }
//
// Generates:
//   func _count_wrapper(): void {
//       while clk.running() {
//           clk.posedge.wait()
//           count()
//       }
//   }
//

#include "state.h"
#include <cxy/ast.h>
#include <cxy/core/mempool.h>
#include <cxy/flag.h>
#include <cxy/core/strpool.h>
#include <cxy/strings.h>
#include <string.h>

// Forward declarations for interned strings
extern cstring rtl_clock;
extern cstring rtl_posedge;
extern cstring rtl_negedge;
extern cstring rtl_valueChanged;
extern cstring rtl_change;
extern cstring rtl_method;
extern cstring rtl_bitsignal;
extern cstring rtl_wait;
extern cstring rtl_waitAny;
extern cstring rtl_waitAll;
extern cstring rtl_running;

// Forward declarations (defined in transform.c)
static void removeAttribute(AstNode *node, cstring attrName);
static void injectMemberIntoComponent(RtlComponentInfo *comp, AstNode *newMember);

// Forward declarations (defined later in this file)
static RtlPortInfo *findPortForCodegen(RtlComponentInfo *comp, cstring name);
static RtlSignalInfo *findSignalForCodegen(RtlComponentInfo *comp, cstring name);
static AstNode *makeThisMember(MemPool *pool, const FileLoc *loc, cstring fieldName);

// ============================================================================
// Helper: Create identifier node
// ============================================================================

static AstNode *makePathForName(MemPool *pool, const FileLoc *loc, cstring name)
{
    return makePath(pool, loc, name, flgNone, NULL);
}

// ============================================================================
// Helper: Create call expression (e.g., func())
// ============================================================================

static AstNode *makeCall(MemPool *pool, const FileLoc *loc, AstNode *callee)
{
    return makeCallExpr(pool, loc, callee, NULL, flgNone, NULL, NULL);
}

// ============================================================================
// Generate while loop condition
// ============================================================================

static AstNode *makeLoopCondition(MemPool *pool,
                                   StrPool *strings,
                                   const FileLoc *loc,
                                   RtlPortInfo *port,
                                   cstring portName)
{
    // For Clock ports: clk.running()
    if (port->isClock) {
        AstNode *runningElem = makeResolvedPathElement(pool, loc, rtl_running, flgNone, NULL, NULL, NULL);
        AstNode *portElem = makeResolvedPathElement(pool, loc, portName, flgNone, NULL, runningElem, NULL);
        AstNode *path = makePathWithElements(pool, loc, flgNone, portElem, NULL);
        return makeCall(pool, loc, path);
    }

    // For signal ports: true (infinite loop)
    return makeBoolLiteral(pool, loc, true, NULL, NULL);
}

// ============================================================================
// Generate event wait statement: port.event.wait()
// ============================================================================

static AstNode *makeEventWaitStmt(MemPool *pool,
                                   StrPool *strings,
                                   const FileLoc *loc,
                                   cstring portName,
                                   cstring eventName)
{
    // Build path: portName.eventName.wait
    AstNode *waitElem = makeResolvedPathElement(pool, loc, rtl_wait, flgNone, NULL, NULL, NULL);
    AstNode *eventElem = makeResolvedPathElement(pool, loc, eventName, flgNone, NULL, waitElem, NULL);
    AstNode *portElem = makeResolvedPathElement(pool, loc, portName, flgNone, NULL, eventElem, NULL);
    AstNode *path = makePathWithElements(pool, loc, flgNone, portElem, NULL);

    // Call: portName.eventName.wait()
    AstNode *waitCall = makeCall(pool, loc, path);

    // Wrap in expression statement
    return makeExprStmt(pool, loc, flgNone, waitCall, NULL, NULL);
}

// ============================================================================
// Generate method call statement: methodName()
// ============================================================================

static AstNode *makeMethodCallStmt(MemPool *pool,
                                    const FileLoc *loc,
                                    cstring methodName)
{
    AstNode *methodIdent = makePathForName(pool, loc, methodName);
    AstNode *methodCall = makeCall(pool, loc, methodIdent);

    // Wrap in expression statement
    return makeExprStmt(pool, loc, flgNone, methodCall, NULL, NULL);
}

// ============================================================================
// Helper: Check if a name refers to a signal
// ============================================================================

static bool isSignal(RtlComponentInfo *comp, cstring name)
{
    for (RtlSignalInfo *signal = comp->signals; signal; signal = signal->next) {
        if (signal->name == name) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Helper: Find signal by name
// ============================================================================

static RtlSignalInfo *findSignalForCodegen(RtlComponentInfo *comp, cstring name)
{
    for (RtlSignalInfo *signal = comp->signals; signal; signal = signal->next) {
        if (signal->name == name) {
            return signal;
        }
    }
    return NULL;
}

// ============================================================================
// Helper: Check if a trigger event refers to an array port or signal
// Returns array dimension expression if array, NULL otherwise
// ============================================================================

static AstNode *getArrayDimExprForTrigger(RtlComponentInfo *comp, cstring name, bool *outIsSignal)
{
    // Check ports
    RtlPortInfo *port = findPortForCodegen(comp, name);
    if (port && port->isArray) {
        if (outIsSignal) *outIsSignal = false;
        return port->arrayDimExpr;
    }

    // Check signals
    RtlSignalInfo *signal = findSignalForCodegen(comp, name);
    if (signal && signal->isArray) {
        if (outIsSignal) *outIsSignal = true;
        return signal->arrayDimExpr;
    }

    return NULL;
}

// ============================================================================
// Helper: Find port by name (local version for codegen)
// ============================================================================

static RtlPortInfo *findPortForCodegen(RtlComponentInfo *comp, cstring name)
{
    for (RtlPortInfo *port = comp->ports; port; port = port->next) {
        if (port->name == name) {
            return port;
        }
    }
    return NULL;
}

// ============================================================================
// Helper: Check if method is combinational (vs sequential/clocked)
// ============================================================================

static bool isCombinationalMethod(RtlComponentInfo *comp, RtlMethodInfo *method)
{
    // A method is combinational if it has triggers but none are clock edges
    if (!method->trigger.events || method->trigger.eventCount == 0) {
        return false;
    }

    // Check if any trigger is a clock edge (posedge/negedge on Clock port)
    for (RtlTriggerEvent *evt = method->trigger.events; evt; evt = evt->next) {
        RtlPortInfo *port = findPortForCodegen(comp, evt->portName);
        if (port && port->isClock) {
            // If triggering on posedge or negedge of a clock, it's sequential
            if (evt->eventName == rtl_posedge || evt->eventName == rtl_negedge) {
                return false;  // Sequential method
            }
        }
    }

    // No clock edges found, so it's combinational
    return true;
}

// ============================================================================
// Generate EventGroup variable declaration
// ============================================================================

static AstNode *makeEventGroupVarDecl(MemPool *pool,
                                       StrPool *strings,
                                       const FileLoc *loc,
                                       RtlTriggerEvent *events,
                                       RtlComponentInfo *comp)
{
    // Build EventGroup constructor argument list: port1.event1, port2.event2, ...
    AstNode *firstArg = NULL;
    AstNode *lastArg = NULL;

    for (RtlTriggerEvent *event = events; event; event = event->next) {
        AstNode *eventExpr = NULL;

        // Check if this is a signal or a port
        bool isSignalEvent = isSignal(comp, event->portName);

        // For signals: all events are fields (valueChanged, etc.)
        // For ports: valueChanged/change are methods, posedge/negedge are fields
        bool needsMethodCall = !isSignalEvent &&
                               (event->eventName == rtl_valueChanged ||
                                event->eventName == rtl_change);

        // Build path: portName.eventName
        AstNode *eventElem = makeResolvedPathElement(pool, loc, event->eventName, flgNone, NULL, NULL, NULL);
        AstNode *portElem = makeResolvedPathElement(pool, loc, event->portName, flgNone, NULL, eventElem, NULL);
        AstNode *path = makePathWithElements(pool, loc, flgNone, portElem, NULL);

        if (needsMethodCall) {
            // Call the method: portName.eventName()
            eventExpr = makeCall(pool, loc, path);
        } else {
            // Direct field access: portName.eventName
            eventExpr = path;
        }

        if (firstArg == NULL) {
            firstArg = eventExpr;
            lastArg = eventExpr;
        } else {
            lastArg->next = eventExpr;
            lastArg = eventExpr;
        }
    }

    // Build EventGroup type path
    AstNode *eventGroupTypePath = makePathForName(pool, loc, rtl_EventGroup);

    // Build constructor call: EventGroup(port1.event1, port2.event2, ...)
    AstNode *constructorCall = makeCallExpr(pool, loc, eventGroupTypePath, firstArg, flgNone, NULL, NULL);

    // Create variable declaration: var event = EventGroup(...)
    cstring varName = makeString(strings, "event");
    return makeVarDecl(pool, loc, flgNone, varName, NULL, constructorCall, NULL, NULL);
}

// ============================================================================
// Generate EventGroup wait statement
// ============================================================================

static AstNode *makeEventGroupWaitStmt(MemPool *pool,
                                        StrPool *strings,
                                        const FileLoc *loc,
                                        RtlTriggerMode mode)
{
    // Determine which wait method to call
    cstring waitMethod = (mode == rtlTriggerAny) ? rtl_waitAny : rtl_waitAll;

    // Build path: event.waitAny() or event.waitAll()
    cstring eventVarName = makeString(strings, "event");
    AstNode *waitElem = makeResolvedPathElement(pool, loc, waitMethod, flgNone, NULL, NULL, NULL);
    AstNode *eventElem = makeResolvedPathElement(pool, loc, eventVarName, flgNone, NULL, waitElem, NULL);
    AstNode *path = makePathWithElements(pool, loc, flgNone, eventElem, NULL);

    // Call: event.waitAny() or event.waitAll()
    AstNode *waitCall = makeCall(pool, loc, path);

    // Wrap in expression statement
    return makeExprStmt(pool, loc, flgNone, waitCall, NULL, NULL);
}

// ============================================================================
// Generate wrapper function for a method
// ============================================================================

AstNode *rtlGenerateWrapper(MemPool *pool,
                             StrPool *strings,
                             RtlComponentInfo *comp,
                             RtlMethodInfo *method)
{
    const FileLoc *loc = &method->funcNode->loc;

    // Generate wrapper name: _{methodName}_wrapper
    cstring wrapperName = makeStringConcat(strings, "_", method->name, "_wrapper");

    AstNode *loopBodyStmts = NULL;
    AstNode *conditionStmt = NULL;
    AstNode *preLoopStmts = NULL;

    if (method->trigger.mode == rtlTriggerSingle) {
        // Single event mode
        // Generate: var event = port.event() or port.event; while { event.wait(); methodName(); }
        // Special case: if port/signal is an array, generate EventGroup with comptime for loop

        RtlTriggerEvent *event = method->trigger.events;
        if (!event) {
            return NULL;  // Should never happen - validation checked this
        }

        // Check if this trigger refers to an array port/signal
        bool isArraySignal = false;
        AstNode *arrayDimExpr = getArrayDimExprForTrigger(comp, event->portName, &isArraySignal);

        if (arrayDimExpr) {
            // Array port/signal - generate EventGroup with comptime for loop
            // Generate:
            //   var eg = EventGroup()
            //   #for (const i: 0..N) {
            //       eg.add(this.portName.[#{i}].eventName())
            //   }
            //   while { eg.wait(); methodName(); }

            cstring egVarName = makeString(strings, "eg");

            // 1. Create EventGroup constructor: var eg = EventGroup()
            AstNode *egTypePath = makePathForName(pool, loc, rtl_EventGroup);
            AstNode *egConstructor = makeCallExpr(pool, loc, egTypePath, NULL, flgNone, NULL, NULL);
            AstNode *egDecl = makeVarDecl(pool, loc, flgNone, egVarName, NULL, egConstructor, NULL, NULL);

            // 2. Create comptime for loop: #for (const i: 0..N) { eg.add(...) }

            // Loop variable: const i
            AstNode *loopVar = makeVarDecl(pool, loc, flgConst, makeString(strings, "i"),
                                            NULL, NULL, NULL, NULL);

            // Range: 0..N
            AstNode *zeroLit = makeIntegerLiteral(pool, loc, 0, NULL, NULL);
            AstNode *dimExpr = deepCloneAstNode(pool, arrayDimExpr);
            AstNode *rangeExpr = makeAstNode(
                pool,
                loc,
                &(AstNode){.tag = astRangeExpr,
                           .flags = flgNone,
                           .rangeExpr = {.start = zeroLit,
                                         .end = dimExpr,
                                         .step = NULL}});

            // Loop body: eg.add(this.portName.[#{i}].eventName())
            
            // this.portName
            AstNode *thisFieldPath = makeThisMember(pool, loc, event->portName);
            
            // this.portName.[#{i}]
            AstNode *iPath = makePathForName(pool, loc, makeString(strings, "i"));
            iPath->flags |= flgComptime;
            AstNode *arrayAccess = makeIndexExpr(pool, loc, flgNone, thisFieldPath, iPath, NULL, NULL);

            // arrayAccess.eventName or arrayAccess.eventName()
            bool isSignalEvent = isArraySignal;
            bool needsMethodCall = !isSignalEvent &&
                                   (event->eventName == rtl_valueChanged ||
                                    event->eventName == rtl_change);

            // Create member access: arrayAccess.eventName
            AstNode *eventIdent = makeIdentifier(pool, loc, event->eventName, 0, NULL, NULL);
            AstNode *memberAccess = makeMemberExpr(pool, loc, flgNone, arrayAccess, eventIdent, NULL, NULL);

            AstNode *eventExpr;
            if (needsMethodCall) {
                eventExpr = makeCall(pool, loc, memberAccess);
            } else {
                eventExpr = memberAccess;
            }

            // eg.addEvent(eventExpr)
            AstNode *egPath = makePathForName(pool, loc, egVarName);
            AstNode *addEventIdent = makeIdentifier(pool, loc, makeString(strings, "addEvent"), 0, NULL, NULL);
            AstNode *addEventPath = makeMemberExpr(pool, loc, flgNone, egPath, addEventIdent, NULL, NULL);
            AstNode *addCall = makeCallExpr(pool, loc, addEventPath, eventExpr, flgNone, NULL, NULL);
            AstNode *addStmt = makeExprStmt(pool, loc, flgNone, addCall, NULL, NULL);

            // Wrap in block
            AstNode *loopBody = makeBlockStmt(pool, loc, addStmt, NULL, NULL);

            // Create comptime for
            AstNode *comptimeFor = makeForStmt(pool, loc, flgComptime, loopVar, rangeExpr, NULL, loopBody, NULL);

            // Link: egDecl -> comptimeFor
            egDecl->next = comptimeFor;
            preLoopStmts = egDecl;

            // Generate infinite loop condition
            conditionStmt = makeBoolLiteral(pool, loc, true, NULL, NULL);

            // Generate loop body: eg.waitAny() + method call
            AstNode *waitAnyIdent = makeIdentifier(pool, loc, rtl_waitAny, 0, NULL, NULL);
            AstNode *egPathForWait = makePathForName(pool, loc, egVarName);
            AstNode *waitAnyMember = makeMemberExpr(pool, loc, flgNone, egPathForWait, waitAnyIdent, NULL, NULL);
            AstNode *waitCall = makeCall(pool, loc, waitAnyMember);
            AstNode *waitStmt = makeExprStmt(pool, loc, flgNone, waitCall, NULL, NULL);

            AstNode *callStmt = makeMethodCallStmt(pool, loc, method->name);
            waitStmt->next = callStmt;
            loopBodyStmts = waitStmt;

        } else {
            // Non-array port/signal - use existing logic
            // Generate event variable declaration
            cstring eventVarName = makeString(strings, "event");

            // Check if this is a signal or a port
            bool isSignalEvent = isSignal(comp, event->portName);

            // For signals: all events are fields (valueChanged, etc.)
            // For ports: valueChanged/change are methods, posedge/negedge are fields
            bool needsMethodCall = !isSignalEvent &&
                                   (event->eventName == rtl_valueChanged ||
                                    event->eventName == rtl_change);

            // Build path: portName.eventName
            AstNode *eventElem = makeResolvedPathElement(pool, loc, event->eventName, flgNone, NULL, NULL, NULL);
            AstNode *portElem = makeResolvedPathElement(pool, loc, event->portName, flgNone, NULL, eventElem, NULL);
            AstNode *path = makePathWithElements(pool, loc, flgNone, portElem, NULL);

            AstNode *eventAccess;
            if (needsMethodCall) {
                // Call the method: portName.eventName()
                eventAccess = makeCall(pool, loc, path);
            } else {
                // Direct field access: portName.eventName
                eventAccess = path;
            }

            // Create variable declaration: var event = port.event() or port.event
            preLoopStmts = makeVarDecl(pool, loc, flgNone, eventVarName, NULL, eventAccess, NULL, NULL);

            // Generate infinite loop condition (true literal)
            conditionStmt = makeBoolLiteral(pool, loc, true, NULL, NULL);

            // Generate loop body: event.wait() + method call
            // Build path: event.wait
            AstNode *waitElem = makeResolvedPathElement(pool, loc, rtl_wait, flgNone, NULL, NULL, NULL);
            AstNode *eventVarElem = makeResolvedPathElement(pool, loc, eventVarName, flgNone, NULL, waitElem, NULL);
            AstNode *waitPath = makePathWithElements(pool, loc, flgNone, eventVarElem, NULL);
            AstNode *waitCall = makeCall(pool, loc, waitPath);
            AstNode *waitStmt = makeExprStmt(pool, loc, flgNone, waitCall, NULL, NULL);

            AstNode *callStmt = makeMethodCallStmt(pool, loc, method->name);
            waitStmt->next = callStmt;
            loopBodyStmts = waitStmt;
        }

    } else {
        // Multiple event mode (ANY or ALL)
        // Generate: var event = EventGroup(...); while { event.waitAny/All(); methodName(); }

        // Generate EventGroup variable declaration
        preLoopStmts = makeEventGroupVarDecl(pool, strings, loc, method->trigger.events, comp);

        // Generate infinite loop condition (true literal)
        conditionStmt = makeBoolLiteral(pool, loc, true, NULL, NULL);

        // Generate loop body: wait + method call
        AstNode *waitStmt = makeEventGroupWaitStmt(pool, strings, loc, method->trigger.mode);
        AstNode *callStmt = makeMethodCallStmt(pool, loc, method->name);
        waitStmt->next = callStmt;
        loopBodyStmts = waitStmt;
    }

    // Create block for while loop body
    AstNode *loopBody = makeBlockStmt(pool, loc, loopBodyStmts, NULL, NULL);

    // Create while statement
    AstNode *whileStmt = makeWhileStmt(pool, loc, flgNone, conditionStmt, loopBody,
                                        NULL, NULL);

    // For combinational methods, call the method once before entering the loop
    // This simulates "always active" combinational logic behavior
    AstNode *initialCallStmt = NULL;
    if (isCombinationalMethod(comp, method)) {
        initialCallStmt = makeMethodCallStmt(pool, loc, method->name);
    }

    // Link pre-loop statements: event declaration -> initial call (if combinational) -> while loop
    AstNode *funcBodyStmts = preLoopStmts;
    if (funcBodyStmts) {
        if (initialCallStmt) {
            // Find end of preLoopStmts chain
            AstNode *last = funcBodyStmts;
            while (last->next) last = last->next;
            last->next = initialCallStmt;
            initialCallStmt->next = whileStmt;
        } else {
            funcBodyStmts->next = whileStmt;
        }
    } else {
        if (initialCallStmt) {
            funcBodyStmts = initialCallStmt;
            initialCallStmt->next = whileStmt;
        } else {
            funcBodyStmts = whileStmt;
        }
    }

    // Create function body block
    AstNode *funcBody = makeBlockStmt(pool, loc, funcBodyStmts, NULL, NULL);

    // Create void return type
    AstNode *returnType = makeVoidAstNode(pool, loc, flgNone, NULL, NULL);

    // Create function declaration
    AstNode *funcDecl = makeFunctionDecl(pool,
                                          loc,
                                          wrapperName,
                                          NULL,        // no parameters
                                          returnType,
                                          funcBody,
                                          flgMember,
                                          NULL,        // next
                                          NULL);       // type

    // Remove @method attribute from original function
    removeAttribute(method->funcNode, rtl_method);

    return funcDecl;
}

// ============================================================================
// Generate all wrappers for a component
// ============================================================================

void rtlGenerateWrappers(MemPool *pool,
                          StrPool *strings,
                          RtlComponentInfo *comp)
{
    AstNode *lastWrapper = NULL;

    for (RtlMethodInfo *method = comp->methods; method; method = method->next) {
        AstNode *wrapper = rtlGenerateWrapper(pool, strings, comp, method);
        if (!wrapper) {
            continue;
        }

        // Link wrappers together
        if (!comp->generatedWrappers) {
            comp->generatedWrappers = wrapper;
            lastWrapper = wrapper;
        } else {
            lastWrapper->next = wrapper;
            lastWrapper = wrapper;
        }
    }

    // Inject wrappers into component's member list
    injectMemberIntoComponent(comp, comp->generatedWrappers);
}

// ============================================================================
// Generate Constructor for Component
// ============================================================================

// Helper: Create this.field member access
static AstNode *makeThisMember(MemPool *pool, const FileLoc *loc, cstring fieldName)
{
    AstNode *fieldElem = makeResolvedPathElement(pool, loc, fieldName, flgNone, NULL, NULL, NULL);
    AstNode *thisElem = makeResolvedPathElement(pool, loc, S_this, flgNone, NULL, fieldElem, NULL);
    return makePathWithElements(pool, loc, flgNone, thisElem, NULL);
}

// Helper: Check if type is already BitSignal[N]
static bool isBitSignalType(const AstNode *typeNode)
{
    if (!typeNode || !nodeIs(typeNode, Path)) {
        return false;
    }

    const AstNode *firstElem = typeNode->path.elements;
    if (!firstElem || !nodeIs(firstElem, PathElem)) {
        return false;
    }

    return firstElem->pathElement.name == rtl_bitsignal;
}

// Helper: Create function parameter for port
// Clock ports: clk: &Clock (value type needs reference)
// BitSignal ports: port: BitSignal[N] (class, already a reference type - no & needed)
// Signal ports: port: Signal[T] (class, already a reference type - no & needed)
static AstNode *makePortParameter(MemPool *pool,
                                  RtlPortInfo *port,
                                  const FileLoc *loc)
{
    AstNode *paramType;

    // Handle array ports: generate tupleof!(Signal[T], N) macro call
    if (port->isArray) {
        // Get element signal type (Signal[T] or BitSignal[W])
        AstNode *elementSignalType;
        if (port->paramType && isBitSignalType(port->paramType)) {
            // BitSignal[W]
            elementSignalType = deepCloneAstNode(pool, port->paramType);
        } else {
            // Signal[T]
            AstNode *innerType = deepCloneAstNode(pool, port->paramType);
            elementSignalType = makeResolvedPathWithArgs(pool, loc, rtl_signal, flgNone, NULL, innerType, NULL);
            elementSignalType->path.isType = true;
        }

        // Set required flags for type argument in tupleof! macro
        elementSignalType->flags |= flgTypeAst | flgTypeinfo;

        // Clone dimension expression
        AstNode *dimExpr = deepCloneAstNode(pool, port->arrayDimExpr);

        // Build argument list: (elementSignalType, dimExpr)
        elementSignalType->next = dimExpr;

        // Create macro identifier: tupleof
        AstNode *macroIdent = makeIdentifier(pool, loc, rtl_tupleof, 0, NULL, NULL);

        // Create macro call: tupleof!(Signal[T], N)
        // Must have TypeAst and Comptime flags like in working examples
        paramType = makeMacroCallAstNode(pool, loc, flgTypeAst | flgComptime, macroIdent, elementSignalType, NULL);

        return makeFunctionParam(pool, loc, port->name, paramType, NULL, flgNone, NULL);
    }

    if (port->isClock) {
        // Clock ports: use field type directly (&Clock)
        paramType = deepCloneAstNode(pool, port->fieldNode->structField.type);
    } else if (port->paramType && isBitSignalType(port->paramType)) {
        // BitSignal ports: use BitSignal[N] directly (no & - class is already a reference)
        paramType = deepCloneAstNode(pool, port->paramType);
    } else {
        // Regular Signal[T] ports: use Signal[T] directly (no & - Signal is a class)
        AstNode *innerType = deepCloneAstNode(pool, port->paramType);
        paramType = makeResolvedPathWithArgs(pool, loc, rtl_signal, flgNone, NULL, innerType, NULL);
        paramType->path.isType = true;
    }

    return makeFunctionParam(pool, loc, port->name, paramType, NULL, flgNone, NULL);
}

// Helper: Create port binding statement
// Clock: this.clk = clk
// InPort/InBitPort: this.port = InPort[T](&&port) or InBitPort[N](&&port)
// Array ports: #for loop with comptime indexing
static AstNode *makePortBinding(MemPool *pool,
                                 StrPool *strings,
                                 RtlPortInfo *port,
                                 const FileLoc *loc)
{
    // Handle array ports: generate comptime for loop
    if (port->isArray) {
        // Generate: #for (const i: 0..N) { this.port.[#{i}] = InPort[T](port.#{i}) }

        // 1. Create loop variable: const i
        AstNode *loopVar = makeVarDecl(pool, loc, flgConst, makeString(strings, "i"),
                                        NULL, NULL, NULL, NULL);

        // 2. Create range: 0..N
        AstNode *zeroLit = makeIntegerLiteral(pool, loc, 0, NULL, NULL);
        AstNode *dimExpr = deepCloneAstNode(pool, port->arrayDimExpr);
        AstNode *rangeExpr = makeAstNode(
            pool,
            loc,
            &(AstNode){.tag = astRangeExpr,
                       .flags = flgNone,
                       .rangeExpr = {.start = zeroLit,
                                     .end = dimExpr,
                                     .step = NULL}});

        // 3. Create loop body assignment: this.port.[#{i}] = InPort[T](port.#{i})

        // Left side: this.port.[#{i}]
        AstNode *fieldAccess = makeThisMember(pool, loc, port->name);
        AstNode *iPath = makePathForName(pool, loc, makeString(strings, "i"));
        iPath->flags |= flgComptime;
        AstNode *lhs = makeIndexExpr(pool, loc, flgNone, fieldAccess, iPath, NULL, NULL);

        // Right side: InPort[T](&&port.#{i})
        // For tuple member access with comptime index
        // Target must be a Path, not just an Identifier
        AstNode *portPath = makePathForName(pool, loc, port->name);
        
        // Create comptime path for i
        AstNode *iPathRhs = makePathForName(pool, loc, makeString(strings, "i"));
        iPathRhs->flags |= flgComptime;
        
        // Create member access: port.#{i}
        AstNode *tupleAccess = makeMemberExpr(pool, loc, flgNone, portPath, iPathRhs, NULL, NULL);

        // Get wrapper name
        cstring wrapperName;
        bool isBitSignal = port->paramType && isBitSignalType(port->paramType);
        switch (port->direction) {
            case rtlPortIn:
                wrapperName = isBitSignal ? rtl_inbitport : rtl_inport;
                break;
            case rtlPortOut:
                wrapperName = isBitSignal ? rtl_outbitport : rtl_outport;
                break;
            case rtlPortInOut:
                wrapperName = isBitSignal ? rtl_inoutbitport : rtl_inoutport;
                break;
            default:
                return NULL;
        }

        // Get wrapper type args
        AstNode *wrapperTypeArgs;
        if (port->wrapperArgs != NULL) {
            wrapperTypeArgs = deepCloneAstNode(pool, port->wrapperArgs);
        } else {
            wrapperTypeArgs = deepCloneAstNode(pool, port->paramType);
        }

        // Create wrapper path: InPort[T]
        AstNode *wrapperPath = makeResolvedPathWithArgs(pool, loc, wrapperName, flgNone, NULL, wrapperTypeArgs, NULL);

        // Create call: InPort[T](port.#{i})
        AstNode *rhs = makeCallExpr(pool, loc, wrapperPath, tupleAccess, flgNone, NULL, NULL);

        // Create assignment statement
        AstNode *assignExpr = makeAssignExpr(pool, loc, flgNone, lhs, opAssign, rhs, NULL, NULL);
        AstNode *assignStmt = makeExprStmt(pool, loc, flgNone, assignExpr, NULL, NULL);

        // Wrap in block statement
        AstNode *blockStmt = makeBlockStmt(pool, loc, assignStmt, NULL, NULL);

        // 4. Create comptime for statement
        AstNode *comptimeFor = makeForStmt(pool, loc, flgComptime, loopVar, rangeExpr, NULL, blockStmt, NULL);

        return comptimeFor;
    }

    AstNode *lhs = makeThisMember(pool, loc, port->name);
    AstNode *rhs;

    if (port->isClock) {
        // Direct assignment: this.clk = clk
        rhs = makePathForName(pool, loc, port->name);
    } else {
        // Wrapped assignment: this.port = InPort[T](&&port) or InBitPort[N](&&port)
        // 1. Create &&port (using opMove)
        AstNode *portIdent = makePathForName(pool, loc, port->name);
        AstNode *moveExpr = makeUnaryExpr(pool, loc, flgNone, true, opMove, portIdent, NULL, NULL);

        // 2. Get wrapper type name (InPort/OutPort/InOutPort or InBitPort/OutBitPort/InOutBitPort)
        cstring wrapperName;
        bool isBitSignal = port->paramType && isBitSignalType(port->paramType);

        switch (port->direction) {
            case rtlPortIn:
                wrapperName = isBitSignal ? rtl_inbitport : rtl_inport;
                break;
            case rtlPortOut:
                wrapperName = isBitSignal ? rtl_outbitport : rtl_outport;
                break;
            case rtlPortInOut:
                wrapperName = isBitSignal ? rtl_inoutbitport : rtl_inoutport;
                break;
            default:
                return NULL;
        }

        // 3. Get the generic args for the wrapper
        // For BitPorts: use wrapperArgs ([N]) if set, otherwise use paramType (BitSignal[N])
        // For regular ports: use paramType
        AstNode *wrapperTypeArgs;
        if (port->wrapperArgs != NULL) {
            // BitPort: use [N] from wrapperArgs
            wrapperTypeArgs = deepCloneAstNode(pool, port->wrapperArgs);
        } else {
            // Regular port: use Signal[T] from paramType
            wrapperTypeArgs = deepCloneAstNode(pool, port->paramType);
        }

        // 4. Create InPort[T] or InBitPort[N] path
        AstNode *wrapperPath = makeResolvedPathWithArgs(pool, loc, wrapperName, flgNone, NULL, wrapperTypeArgs, NULL);

        // 5. Create call: InPort[T](&&port) or InBitPort[N](&&port)
        rhs = makeCallExpr(pool, loc, wrapperPath, moveExpr, flgNone, NULL, NULL);
    }

    // Create assignment: lhs = rhs
    return makeAssignExpr(pool, loc, flgNone, lhs, opAssign, rhs, NULL, NULL);
}

// Helper: Create async launch for wrapper
// async _methodName_wrapper()
static AstNode *makeAsyncLaunch(MemPool *pool,
                                 StrPool *strings,
                                 RtlMethodInfo *method,
                                 const FileLoc *loc)
{
    // Create wrapper name: _methodName_wrapper
    cstring wrapperName = makeStringConcat(strings, "_", method->name, "_wrapper", NULL);
    AstNode *wrapperIdent = makePathForName(pool, loc, wrapperName);

    // Create call: _methodName_wrapper()
    AstNode *callExpr = makeCallExpr(pool, loc, wrapperIdent, NULL, flgNone, NULL, NULL);

    // Wrap in async: async _methodName_wrapper()
    AstNode *nameArg = makeStringLiteral(pool, loc, method->name, NULL, NULL);
    callExpr->next = nameArg;
    AstNode *asyncMacro = makeIdentifier(pool, loc, S___async, 0, NULL, NULL);
    return makeMacroCallAstNode(pool, loc, flgNone, asyncMacro, callExpr, NULL);
}

// Helper: Create signal initialization statement
// _signalName = Signal[Type](defaultValue, "signalName")
static AstNode *makeSignalInit(MemPool *pool, StrPool *strings, RtlSignalInfo *signal, const FileLoc *loc)
{
    // Handle array signals: generate comptime for loop
    if (signal->isArray) {
        // Generate: #for (const i: 0..N) { this.signal.[#{i}] = Signal[T](defaultValue, "signal") }

        // 1. Create loop variable: const i
        AstNode *loopVar = makeVarDecl(pool, loc, flgConst, makeString(strings, "i"),
                                        NULL, NULL, NULL, NULL);

        // 2. Create range: 0..N
        AstNode *zeroLit = makeIntegerLiteral(pool, loc, 0, NULL, NULL);
        AstNode *dimExpr = deepCloneAstNode(pool, signal->arrayDimExpr);
        AstNode *rangeExpr = makeAstNode(
            pool,
            loc,
            &(AstNode){.tag = astRangeExpr,
                       .flags = flgNone,
                       .rangeExpr = {.start = zeroLit,
                                     .end = dimExpr,
                                     .step = NULL}});

        // 3. Create loop body assignment: this.signal.[#{i}] = Signal[T](defaultValue, "signal")

        // Left side: this.signal.[#{i}]
        AstNode *fieldAccess = makeThisMember(pool, loc, signal->name);
        AstNode *iPath = makePathForName(pool, loc, makeString(strings, "i"));
        iPath->flags |= flgComptime;
        AstNode *lhs = makeIndexExpr(pool, loc, flgNone, fieldAccess, iPath, NULL, NULL);

        // Right side: get element type and create Signal/BitSignal constructor
        // Field type is [Signal[T], N] or [BitSignal[N], N]
        // We need to extract Signal[T] or BitSignal[N]
        AstNode *fieldType = signal->fieldNode->structField.type;
        AstNode *elementType = NULL;
        
        if (nodeIs(fieldType, ArrayType)) {
            elementType = fieldType->arrayType.elementType;
        } else {
            // Shouldn't happen, but fallback
            elementType = fieldType;
        }

        // Check if element is BitSignal[N] or Signal[T]
        bool isBitSignal = false;
        AstNode *genericArgs = NULL;
        
        if (nodeIs(elementType, Path) && elementType->path.elements) {
            const AstNode *firstElem = elementType->path.elements;
            if (firstElem->pathElement.name == rtl_bitsignal) {
                isBitSignal = true;
                genericArgs = firstElem->pathElement.args;
            } else if (firstElem->pathElement.name == rtl_signal) {
                genericArgs = firstElem->pathElement.args;
            }
        }

        AstNode *signalTypePath;
        if (isBitSignal) {
            signalTypePath = makeResolvedPathWithArgs(pool, loc, rtl_bitsignal, flgNone, NULL,
                                                       deepCloneAstNode(pool, genericArgs), NULL);
        } else {
            signalTypePath = makeResolvedPathWithArgs(pool, loc, rtl_signal, flgNone, NULL,
                                                       deepCloneAstNode(pool, genericArgs), NULL);
        }

        // Clone the default value expression
        AstNode *defaultValue = deepCloneAstNode(pool, signal->defaultValue);

        // Create string literal for signal name
        AstNode *nameArg = makeStringLiteral(pool, loc, signal->name, NULL, NULL);

        // Link defaultValue -> nameArg
        defaultValue->next = nameArg;

        // Create call: Signal[T](defaultValue, "signal") or BitSignal[N](defaultValue, "signal")
        AstNode *rhs = makeCallExpr(pool, loc, signalTypePath, defaultValue, flgNone, NULL, NULL);

        // Create assignment statement
        AstNode *assignExpr = makeAssignExpr(pool, loc, flgNone, lhs, opAssign, rhs, NULL, NULL);
        AstNode *assignStmt = makeExprStmt(pool, loc, flgNone, assignExpr, NULL, NULL);

        // Wrap in block statement
        AstNode *blockStmt = makeBlockStmt(pool, loc, assignStmt, NULL, NULL);

        // 4. Create comptime for statement
        AstNode *comptimeFor = makeForStmt(pool, loc, flgComptime, loopVar, rangeExpr, NULL, blockStmt, NULL);

        return comptimeFor;
    }

    // Single signal (non-array)
    // Left side: _signalName
    AstNode *lhs = makePathForName(pool, loc, signal->name);

    // Get the field type (either Signal[T] or BitSignal[N])
    AstNode *fieldType = signal->fieldNode->structField.type;
    
    // Check if this is BitSignal[N] (not Signal[T])
    bool isBitSignal = false;
    AstNode *genericArgs = NULL;
    
    if (nodeIs(fieldType, Path) && fieldType->path.elements) {
        const AstNode *firstElem = fieldType->path.elements;
        if (firstElem->pathElement.name == rtl_bitsignal) {
            isBitSignal = true;
            genericArgs = firstElem->pathElement.args;
        }
    }
    
    AstNode *signalTypePath;
    if (isBitSignal) {
        // BitSignal[N] case
        signalTypePath = makeResolvedPathWithArgs(pool, loc, rtl_bitsignal, flgNone, NULL,
                                                   deepCloneAstNode(pool, genericArgs), NULL);
    } else {
        // Signal[T] case (existing logic)
        AstNode *innerType = fieldType;
        
        // Extract the inner type from Signal[T] wrapper
        // The type has already been transformed to Signal[T], so we need to get T
        if (nodeIs(innerType, Path) && innerType->path.elements &&
            innerType->path.elements->pathElement.args) {
            innerType = innerType->path.elements->pathElement.args;
        }
        
        // Create Signal[T] path
        signalTypePath = makeResolvedPathWithArgs(pool, loc, rtl_signal, flgNone, NULL,
                                                   deepCloneAstNode(pool, innerType), NULL);
    }

    // Clone the default value expression
    AstNode *defaultValue = deepCloneAstNode(pool, signal->defaultValue);

    // Create string literal for signal name
    AstNode *nameArg = makeStringLiteral(pool, loc, signal->name, NULL, NULL);

    // Link defaultValue -> nameArg to form the argument list
    defaultValue->next = nameArg;

    // Create call: Signal[T](defaultValue, "signalName") or BitSignal[N](defaultValue, "signalName")
    AstNode *rhs = makeCallExpr(pool, loc, signalTypePath, defaultValue, flgNone, NULL, NULL);

    // Create assignment: _signalName = Signal[T](...) or BitSignal[N](...)
    return makeAssignExpr(pool, loc, flgNone, lhs, opAssign, rhs, NULL, NULL);
}

void rtlGenerateConstructor(MemPool *pool,
                             StrPool *strings,
                             RtlComponentInfo *comp)
{
    if (!comp->ports && !comp->signals) {
        return; // No ports or signals, no constructor needed
    }

    const FileLoc *loc = &comp->structNode->loc;

    // 1. Generate parameters (only for ports, not signals)
    AstNode *params = NULL;
    AstNode *lastParam = NULL;

    for (RtlPortInfo *port = comp->ports; port; port = port->next) {
        AstNode *param = makePortParameter(pool, port, loc);
        if (!param) continue;

        if (!params) {
            params = param;
            lastParam = param;
        } else {
            lastParam->next = param;
            lastParam = param;
        }
    }

    // 2. Generate body statements
    AstNode *body = NULL;
    AstNode *lastStmt = NULL;

    // Signal initializations (before port bindings)
    for (RtlSignalInfo *signal = comp->signals; signal; signal = signal->next) {
        AstNode *init = makeSignalInit(pool, strings, signal, loc);
        if (!init) continue;

        // Check if init is already a statement (e.g., ForStmt for array signals)
        // or an expression that needs wrapping (e.g., AssignExpr for regular signals)
        AstNode *stmt;
        if (init->tag == astForStmt || init->tag == astExprStmt) {
            // Already a statement, use as-is
            stmt = init;
        } else {
            // Expression, wrap in ExprStmt
            stmt = makeExprStmt(pool, loc, flgNone, init, NULL, NULL);
        }

        if (!body) {
            body = stmt;
            lastStmt = stmt;
        } else {
            lastStmt->next = stmt;
            lastStmt = stmt;
        }
    }

    // Port bindings
    for (RtlPortInfo *port = comp->ports; port; port = port->next) {
        AstNode *binding = makePortBinding(pool, strings, port, loc);
        if (!binding) continue;

        // Check if binding is already a statement (e.g., ForStmt for array ports)
        // or an expression that needs wrapping (e.g., AssignExpr for regular ports)
        AstNode *stmt;
        if (binding->tag == astForStmt || binding->tag == astExprStmt) {
            // Already a statement, use as-is
            stmt = binding;
        } else {
            // Expression, wrap in ExprStmt
            stmt = makeExprStmt(pool, loc, flgNone, binding, NULL, NULL);
        }

        if (!body) {
            body = stmt;
            lastStmt = stmt;
        } else {
            lastStmt->next = stmt;
            lastStmt = stmt;
        }
    }

    // Async wrapper launches
    for (RtlMethodInfo *method = comp->methods; method; method = method->next) {
        AstNode *launch = makeAsyncLaunch(pool, strings, method, loc);

        // Wrap in expression statement
        AstNode *exprStmt = makeExprStmt(pool, loc, flgNone, launch, NULL, NULL);

        if (!body) {
            body = exprStmt;
            lastStmt = exprStmt;
        } else {
            lastStmt->next = exprStmt;
            lastStmt = exprStmt;
        }
    }

    // 3. Create body block
    AstNode *bodyBlock = makeBlockStmt(pool, loc, body, NULL, NULL);

    // 4. Create function: func init(...) { ... }
    AstNode *returnType = makeVoidAstNode(pool, loc, flgNone, NULL, NULL);

    AstNode *funcDecl = makeFunctionDecl(pool,
                                          loc,
                                          S_InitOverload,
                                          params,
                                          returnType,
                                          bodyBlock,
                                          flgPublic | flgMember,
                                          NULL,
                                          NULL);

    // 5. Mark as init overload
    funcDecl->funcDecl.operatorOverload = opInitOverload;

    // 6. Inject into component
    injectMemberIntoComponent(comp, funcDecl);
}
