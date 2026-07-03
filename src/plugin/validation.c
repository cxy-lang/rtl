//
// RTL Plugin - Validation Pass
//
// Validates that attributes are used correctly:
// - @input/@output/@inout only on fields within components
// - @method only on functions within components
// - Port types are valid
// - Trigger references exist and event names are valid
//

#include "state.h"
#include <cxy/ast.h>
#include <cxy/core/log.h>

// ============================================================================
// Validation: Port Types
// ============================================================================

static bool validatePortType(Log *L, const AstNode *typeNode, const FileLoc *loc)
{
    if (!typeNode) {
        logError(L, loc, "port has no type specified", NULL);
        return false;
    }
    
    // Clock is always valid
    if (nodeIs(typeNode, ReferenceType)) {
        const AstNode *referred = typeNode->referenceType.referred;
        if (referred && nodeIs(referred, Path) && referred->path.elements &&
            referred->path.elements->pathElement.name == rtl_clock) {
            return true;  // &Clock is valid
        }
    }
    
    // For now, accept any other type - will be wrapped in InPort/OutPort/InOutPort
    // TODO: Could add more specific validation (e.g., must be Signal[T] compatible)
    return true;
}

// ============================================================================
// Validation: Trigger Specifications
// ============================================================================

static bool validateTriggerEvent(Log *L, 
                                  const char *eventName, 
                                  bool isClock,
                                  const FileLoc *loc)
{
    if (!eventName) {
        logError(L, loc, "@method requires trigger specification (e.g., @method(clk: \"posedge\"))", NULL);
        return false;
    }
    
    // Valid clock events
    // eventName comes from AST and is interned, so use == comparison
    if (isClock) {
        if (eventName == rtl_posedge || eventName == rtl_negedge) {
            return true;
        }
        logError(L, loc, "invalid clock event '{s}' - expected 'posedge' or 'negedge'",
                 (FormatArg[]){{.s = eventName}});
        return false;
    }
    
    // Valid signal events
    if (eventName == rtl_valueChanged || eventName == rtl_change) {
        return true;
    }
    
    logError(L, loc, "invalid signal event '{s}' - expected 'valueChanged' or 'change'",
             (FormatArg[]){{.s = eventName}});
    return false;
}

// ============================================================================
// Validation: Method Triggers Reference Valid Ports
// ============================================================================

static RtlPortInfo *findPort(RtlComponentInfo *comp, const char *portName)
{
    for (RtlPortInfo *port = comp->ports; port; port = port->next) {
        if (port->name == portName) {
            return port;
        }
    }
    return NULL;
}

static RtlSignalInfo *findSignal(RtlComponentInfo *comp, const char *signalName)
{
    for (RtlSignalInfo *signal = comp->signals; signal; signal = signal->next) {
        if (signal->name == signalName) {
            return signal;
        }
    }
    return NULL;
}

static bool validateMethodTrigger(Log *L,
                                   RtlMethodInfo *method,
                                   RtlComponentInfo *comp,
                                   const FileLoc *loc)
{
    // Trigger should have been parsed in discovery pass
    if (!method->trigger.events || method->trigger.eventCount == 0) {
        // Error already logged during discovery
        return false;
    }
    
    // Validate each trigger event
    bool valid = true;
    for (RtlTriggerEvent *event = method->trigger.events; event; event = event->next) {
        // Check if it's a port
        RtlPortInfo *port = findPort(comp, event->portName);
        if (port) {
            // Validate event name matches port type
            if (!validateTriggerEvent(L, event->eventName, port->isClock, loc)) {
                valid = false;
            }
            continue;
        }
        
        // Check if it's a signal
        RtlSignalInfo *signal = findSignal(comp, event->portName);
        if (signal) {
            // Signals are not clocks, validate as signal event
            if (!validateTriggerEvent(L, event->eventName, false, loc)) {
                valid = false;
            }
            continue;
        }
        
        // Neither port nor signal found
        logError(L, loc, "method trigger references unknown port or signal '{s}'",
                 (FormatArg[]){{.s = event->portName}});
        valid = false;
    }
    
    return valid;
}

// ============================================================================
// Main Validation Entry Point
// ============================================================================

bool rtlValidateComponent(Log *L, RtlComponentInfo *comp)
{
    bool valid = true;
    
    // Validate each port
    for (RtlPortInfo *port = comp->ports; port; port = port->next) {
        const FileLoc *loc = &port->fieldNode->loc;
        const AstNode *typeNode = port->fieldNode->structField.type;
        if (!validatePortType(L, typeNode, loc)) {
            valid = false;
        }
    }
    
    // Validate each signal has a default value
    for (RtlSignalInfo *signal = comp->signals; signal; signal = signal->next) {
        const FileLoc *loc = &signal->fieldNode->loc;
        if (!signal->defaultValue) {
            logError(L, loc, "@signal field '{s}' must have a default value (e.g., @signal _state: u8 = 0)",
                     (FormatArg[]){{.s = signal->name}});
            valid = false;
        }
    }
    
    // Validate each method
    for (RtlMethodInfo *method = comp->methods; method; method = method->next) {
        const FileLoc *loc = &method->funcNode->loc;
        if (!validateMethodTrigger(L, method, comp, loc)) {
            valid = false;
        }
    }
    
    return valid;
}
