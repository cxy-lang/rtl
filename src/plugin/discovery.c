//
// RTL Plugin - Discovery Pass
//
// Scans the AST node passed to @rtl::component and extracts:
// - Component name (struct/class name)
// - Ports (@in, @out, @inout attributed fields)
// - Methods (@method attributed functions with triggers)
//

#include "state.h"
#include <cxy/ast.h>
#include <cxy/core/strpool.h>
#include <cxy/core/mempool.h>
#include <cxy/core/log.h>
#include <string.h>

// ============================================================================
// Port discovery
// ============================================================================

static RtlPortDirection getPortDirection(const AstNode *field)
{
    if (findAttribute(field, rtl_input))
        return rtlPortIn;
    if (findAttribute(field, rtl_output))
        return rtlPortOut;
    if (findAttribute(field, rtl_inout))
        return rtlPortInOut;
    return rtlPortNone;
}

static bool isClockType(const AstNode *typeNode)
{
    if (!typeNode)
        return false;

    // Check for &Clock pattern (ReferenceType)
    if (nodeIs(typeNode, ReferenceType)) {
        const AstNode *referred = typeNode->referenceType.referred;
        if (referred && nodeIs(referred, Path) && referred->path.elements) {
            return referred->path.elements->pathElement.name == rtl_clock;
        }
    }

    // Check for direct Clock type (Path)
    if (nodeIs(typeNode, Path) && typeNode->path.elements) {
        return typeNode->path.elements->pathElement.name == rtl_clock;
    }

    return false;
}

static RtlPortInfo *discoverPort(MemPool *pool,
                                  AstNode *field,
                                  RtlPortDirection direction)
{
    RtlPortInfo *port = allocFromMemPool(pool, sizeof(RtlPortInfo));
    memset(port, 0, sizeof(RtlPortInfo));

    port->name = field->structField.name;
    port->direction = direction;
    port->isClock = isClockType(field->structField.type);
    port->fieldNode = field;  // Store mutable field node

    // Store original parameter type (Signal[T] or Clock) before transformation
    port->paramType = field->structField.type;
    port->wrapperArgs = NULL;  // Will be set during transformation for BitVector ports

    port->next = NULL;
    return port;
}

// ============================================================================
// Signal discovery
// ============================================================================

static RtlSignalInfo *discoverSignal(MemPool *pool, AstNode *field)
{
    RtlSignalInfo *signal = allocFromMemPool(pool, sizeof(RtlSignalInfo));
    memset(signal, 0, sizeof(RtlSignalInfo));

    signal->name = field->structField.name;
    signal->fieldNode = field;  // Store mutable field node
    signal->defaultValue = field->structField.value;  // Extract default value
    signal->next = NULL;
    return signal;
}

// ============================================================================
// Method discovery
// ============================================================================

static RtlMethodInfo *discoverMethod(MemPool *pool,
                                      AstNode *funcNode,
                                      Log *L,
                                      const FileLoc *loc)
{
    RtlMethodInfo *method = allocFromMemPool(pool, sizeof(RtlMethodInfo));
    memset(method, 0, sizeof(RtlMethodInfo));

    method->name = getDeclarationName(funcNode);
    method->funcNode = funcNode;  // Store mutable function node

    // Parse @method attribute arguments
    // Formats supported:
    // 1. @method(clk: "posedge") - single event, explicit event name
    // 2. @method(reset) - single event, defaults to "change"
    // 3. @method(rst, en) - multiple events with ANY semantics (waitAny)
    // 4. @method(_and, clk, en) - multiple events with ALL semantics (waitAll)
    // 5. @method(rst, clk: "posedge") - mixed syntax
    const AstNode *methodAttr = findAttribute(funcNode, rtl_method);
    if (methodAttr && methodAttr->attr.kvpArgs) {
        const AstNode *arg = methodAttr->attr.args;

        // Check if first argument is "_and" marker
        bool isAndMode = false;
        if (arg && nodeIs(arg, FieldExpr) && arg->fieldExpr.name == rtl_and) {
            isAndMode = true;
            arg = arg->next;  // Skip _and marker
        }

        // Count events and determine mode
        int eventCount = 0;
        const AstNode *countArg = arg;
        while (countArg) {
            if (nodeIs(countArg, FieldExpr)) {
                eventCount++;
            }
            countArg = countArg->next;
        }

        // Set trigger mode based on event count and _and marker
        if (eventCount == 0) {
            logError(L, loc, "@method requires at least one trigger specification", NULL);
            method->trigger.mode = rtlTriggerSingle;
            method->trigger.events = NULL;
            method->trigger.eventCount = 0;
        } else if (eventCount == 1) {
            // Single event - use SINGLE mode for backward compatibility
            method->trigger.mode = rtlTriggerSingle;
            method->trigger.eventCount = 1;

            RtlTriggerEvent *event = allocFromMemPool(pool, sizeof(RtlTriggerEvent));
            event->portName = arg->fieldExpr.name;

            const AstNode *value = arg->fieldExpr.value;
            if (value && nodeIs(value, StringLit)) {
                event->eventName = value->stringLiteral.value;
            } else if (value && nodeIs(value, BoolLit)) {
                event->eventName = rtl_valueChanged;
            } else if (!value) {
                // No event specified, default to valueChanged
                event->eventName = rtl_valueChanged;
            } else {
                logError(L, loc, "@method trigger event must be a string literal or boolean shorthand", NULL);
                event->eventName = NULL;
            }

            event->next = NULL;
            method->trigger.events = event;
        } else {
            // Multiple events - use ANY or ALL mode
            method->trigger.mode = isAndMode ? rtlTriggerAll : rtlTriggerAny;
            method->trigger.eventCount = eventCount;

            // Build linked list of trigger events
            RtlTriggerEvent *firstEvent = NULL;
            RtlTriggerEvent *lastEvent = NULL;

            while (arg) {
                if (nodeIs(arg, FieldExpr)) {
                    RtlTriggerEvent *event = allocFromMemPool(pool, sizeof(RtlTriggerEvent));
                    event->portName = arg->fieldExpr.name;

                    const AstNode *value = arg->fieldExpr.value;
                    if (value && nodeIs(value, StringLit)) {
                        event->eventName = value->stringLiteral.value;
                    } else if (value && nodeIs(value, BoolLit)) {
                        event->eventName = rtl_valueChanged;
                    } else {
                        logError(L, loc, "@method trigger event must be a string literal or boolean shorthand", NULL);
                        event->eventName = NULL;
                    }

                    event->next = NULL;

                    if (firstEvent == NULL) {
                        firstEvent = event;
                        lastEvent = event;
                    } else {
                        lastEvent->next = event;
                        lastEvent = event;
                    }
                }
                arg = arg->next;
            }

            method->trigger.events = firstEvent;
        }
    } else {
        // No arguments or wrong format
        logError(L, loc, "@method requires trigger specification (e.g., @method(clk: \"posedge\"))", NULL);
        method->trigger.mode = rtlTriggerSingle;
        method->trigger.events = NULL;
        method->trigger.eventCount = 0;
    }

    method->next = NULL;
    return method;
}

// ============================================================================
// Component discovery
// ============================================================================

RtlComponentInfo *rtlDiscoverComponent(CxyPluginContext *ctx,
                                        AstNode *classNode,
                                        Log *L,
                                        const FileLoc *loc)
{
    // Validate that we received a class or struct declaration
    if (!classNode || (classNode->tag != astClassDecl && classNode->tag != astStructDecl)) {
        logError(L, loc, "rtl::component! expects a class or struct declaration", NULL);
        return NULL;
    }

    // Allocate component info
    RtlComponentInfo *comp = allocFromMemPool(ctx->pool, sizeof(RtlComponentInfo));
    memset(comp, 0, sizeof(RtlComponentInfo));

    comp->structNode = classNode;
    comp->name = getDeclarationName(classNode);
    comp->portCount = 0;
    comp->signalCount = 0;
    comp->methodCount = 0;
    comp->generatedWrappers = NULL;

    // Discover ports by scanning fields
    RtlPortInfo *lastPort = NULL;
    RtlSignalInfo *lastSignal = NULL;
    AstNode *members = nodeIs(classNode, ClassDecl)
                              ? classNode->classDecl.members
                              : classNode->structDecl.members;

    for (AstNode *member = members; member; member = member->next) {
        if (member->tag == astFieldDecl) {
            RtlPortDirection dir = getPortDirection(member);
            if (dir != rtlPortNone) {
                RtlPortInfo *port = discoverPort(ctx->pool, member, dir);
                if (!comp->ports) {
                    comp->ports = port;
                    lastPort = port;
                } else {
                    lastPort->next = port;
                    lastPort = port;
                }
                comp->portCount++;
            } else if (findAttribute(member, rtl_signal_attr)) {
                // Discover @signal fields
                RtlSignalInfo *signal = discoverSignal(ctx->pool, member);
                if (!comp->signals) {
                    comp->signals = signal;
                    lastSignal = signal;
                } else {
                    lastSignal->next = signal;
                    lastSignal = signal;
                }
                comp->signalCount++;
            }
        }
    }

    // Discover methods by scanning functions with @method attribute
    RtlMethodInfo *lastMethod = NULL;
    for (AstNode *member = members; member; member = member->next) {
        if (nodeIs(member, FuncDecl) && findAttribute(member, rtl_method)) {
            RtlMethodInfo *method = discoverMethod(ctx->pool, member, L, loc);
            if (!comp->methods) {
                comp->methods = method;
                lastMethod = method;
            } else {
                lastMethod->next = method;
                lastMethod = method;
            }
            comp->methodCount++;
        }
    }

    comp->next = NULL;
    return comp;
}
