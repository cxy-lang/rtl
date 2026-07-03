//
// RTL Plugin - Type Transformation Pass
//
// Transforms port field types by wrapping them in InPort/OutPort/InOutPort:
// - @input T → InPort[T] (except Clock which stays as Clock or &Clock)
// - @output T → OutPort[T]
// - @inout T → InOutPort[T]
//
// Also removes @input/@output/@inout attributes from fields.
//

#include "state.h"
#include <cxy/ast.h>
#include <cxy/core/mempool.h>
#include <cxy/flag.h>
#include <string.h>

// Forward declarations for interned strings
extern cstring rtl_input;
extern cstring rtl_output;
extern cstring rtl_inout;
extern cstring rtl_inport;
extern cstring rtl_outport;
extern cstring rtl_inoutport;
extern cstring rtl_inbitport;
extern cstring rtl_outbitport;
extern cstring rtl_inoutbitport;
extern cstring rtl_bitvector;
extern cstring rtl_bitsignal;

// ============================================================================
// AST Node Creation Helpers
// ============================================================================

// Create a generic type wrapper: WrapperName[InnerType]
// e.g., InPort[Bit] → Path{ elements: PathElement{ name: "InPort", args: Bit } }
static AstNode *makePortTypeWrapper(MemPool *pool,
                                     const FileLoc *loc,
                                     cstring wrapperName,
                                     AstNode *innerType)
{
    // Create path element with generic argument
    // PathElement{ name: "InPort", args: innerType }
    AstNode *pathNode = makeResolvedPathWithArgs(pool,
                                         loc,
                                         wrapperName,
                                         flgNone,     // flags
                                         NULL,        // resolveTo
                                         innerType,  // args (the type parameter)
                                         NULL);      // next
    pathNode->path.isType = true;

    return pathNode;
}

// ============================================================================
// BitVector Type Detection and Transformation
// ============================================================================

// Check if a type node is BitVector[N] by examining the path's first element name
static bool isBitVectorType(const AstNode *typeNode)
{
    if (!typeNode || !nodeIs(typeNode, Path)) {
        return false;
    }

    // Get first path element
    const AstNode *firstElem = typeNode->path.elements;
    if (!firstElem || !nodeIs(firstElem, PathElem)) {
        return false;
    }

    // Check if name is "BitVector" (using interned string comparison)
    return firstElem->pathElement.name == rtl_bitvector;
}

// Extract generic args from BitVector[N] type
// Returns the args node that can be reused for BitSignal[N]
static AstNode *extractGenericArgs(const AstNode *typeNode)
{
    if (!typeNode || !nodeIs(typeNode, Path)) {
        return NULL;
    }

    const AstNode *firstElem = typeNode->path.elements;
    if (!firstElem || !nodeIs(firstElem, PathElem)) {
        return NULL;
    }

    // Return the generic args (e.g., [N] from BitVector[N])
    return firstElem->pathElement.args;
}

// Create BitSignal[...] type with the given generic args
// e.g., genericArgs = [N] → creates Path for "BitSignal[N]"
static AstNode *makeBitSignalType(MemPool *pool,
                                   const FileLoc *loc,
                                   AstNode *genericArgs)
{
    // Create path with BitSignal name and generic args
    AstNode *pathNode = makeResolvedPathWithArgs(pool,
                                                  loc,
                                                  rtl_bitsignal,  // "BitSignal"
                                                  flgNone,
                                                  NULL,           // resolveTo
                                                  genericArgs,    // args (e.g., [N])
                                                  NULL);          // next
    pathNode->path.isType = true;
    return pathNode;
}

// Remove an attribute from a node's attribute list
static void removeAttribute(AstNode *node, cstring attrName)
{
    if (!node->attrs) {
        return;
    }

    AstNode **curr = &node->attrs;
    while (*curr) {
        if ((*curr)->attr.name == attrName) {
            // Skip this node by pointing to next
            *curr = (*curr)->next;
            return;
        }
        curr = &(*curr)->next;
    }
}

// Inject a member (or chain of members) into a component's member list
static void injectMemberIntoComponent(RtlComponentInfo *comp, AstNode *newMember)
{
    if (!newMember) {
        return;
    }

    // Get the struct/class members
    AstNode *membersPtr = nodeIs(comp->structNode, ClassDecl)
                            ? comp->structNode->classDecl.members
                            : comp->structNode->structDecl.members;

    // Find the last member
    AstNode *lastMember = getLastAstNode(membersPtr);
    if (lastMember) {
        // Append new member(s) after last
        lastMember->next = newMember;
    } else {
        // No members yet, new member becomes first
        if (nodeIs(comp->structNode, ClassDecl))
            comp->structNode->classDecl.members = newMember;
        else
            comp->structNode->structDecl.members = newMember;
    }
}

// ============================================================================
// Field Type Transformation
// ============================================================================

static void transformPortField(MemPool *pool, RtlPortInfo *port, StrPool *strings)
{
    AstNode *field = port->fieldNode;

    // Clock ports: ensure they are reference types (&Clock)
    if (port->isClock) {
        // If not already a reference type, wrap it
        if (!nodeIs(field->structField.type, ReferenceType)) {
            AstNode *clockType = field->structField.type;
            field->structField.type = makeReferenceTypeAstNode(pool, &field->loc, flgNone, clockType, NULL, NULL);
        }
        // Remove the @input attribute
        removeAttribute(field, rtl_input);
        return;
    }

    // Get the wrapper name based on direction
    cstring wrapperName = NULL;
    cstring attrToRemove = NULL;

    // Get the current type
    AstNode *currentType = field->structField.type;

    // Check if this is a BitVector type - if so, transform to InBitPort[N] with BitSignal[N] param
    if (isBitVectorType(currentType)) {
        // Extract generic args from BitVector[N]
        AstNode *genericArgs = extractGenericArgs(currentType);

        // Clone args for BitSignal[N] type (constructor parameter)
        AstNode *bitSignalArgs = deepCloneAstNode(pool, genericArgs);
        AstNode *bitSignalType = makeBitSignalType(pool, &field->loc, bitSignalArgs);

        // Clone args again for BitPort[N] wrapper (field type)
        AstNode *bitPortArgs = deepCloneAstNode(pool, genericArgs);

        // Clone args yet again for constructor binding (can't reuse)
        AstNode *bindingArgs = deepCloneAstNode(pool, genericArgs);

        // Choose BitPort wrapper based on direction
        switch (port->direction) {
            case rtlPortIn:
                wrapperName = rtl_inbitport;
                attrToRemove = rtl_input;
                break;
            case rtlPortOut:
                wrapperName = rtl_outbitport;
                attrToRemove = rtl_output;
                break;
            case rtlPortInOut:
                wrapperName = rtl_inoutbitport;
                attrToRemove = rtl_inout;
                break;
            default:
                return;
        }

        // Wrap with BitPort[N] (not BitPort[BitSignal[N]]!)
        // InBitPort[N] internally holds BitSignal[N]
        AstNode *wrappedType = makePortTypeWrapper(pool,
                                                    &field->loc,
                                                    wrapperName,
                                                    bitPortArgs);  // Use N, not BitSignal[N]

        // Update field type
        field->structField.type = wrappedType;

        // Store BitSignal[N] as param type (constructor expects &BitSignal[N])
        port->paramType = bitSignalType;

        // Store width args for wrapper construction in constructor body
        port->wrapperArgs = bindingArgs;

        // Remove the port direction attribute
        removeAttribute(field, attrToRemove);
        return;
    }

    // Default: Regular Signal port wrapping
    switch (port->direction) {
        case rtlPortIn:
            wrapperName = rtl_inport;
            attrToRemove = rtl_input;
            break;
        case rtlPortOut:
            wrapperName = rtl_outport;
            attrToRemove = rtl_output;
            break;
        case rtlPortInOut:
            wrapperName = rtl_inoutport;
            attrToRemove = rtl_inout;
            break;
        default:
            return;  // Should never happen after validation
    }

    // Default: Wrap current type in WrapperName[CurrentType]
    AstNode *wrappedType = makePortTypeWrapper(pool,
                                                &field->loc,
                                                wrapperName,
                                                currentType);

    // Replace the field's type with the wrapped version
    field->structField.type = wrappedType;

    // Remove the port direction attribute (@input/@output/@inout)
    removeAttribute(field, attrToRemove);
}

static void transformSignalField(MemPool *pool, RtlSignalInfo *signal, StrPool *strings)
{
    AstNode *field = signal->fieldNode;

    // Get the current type
    AstNode *currentType = field->structField.type;

    // Create wrapped type: Signal[CurrentType]
    AstNode *wrappedType = makePortTypeWrapper(pool,
                                                &field->loc,
                                                rtl_signal,
                                                currentType);

    // Replace the field's type with the wrapped version
    field->structField.type = wrappedType;

    // Remove the default value - it will be initialized in constructor
    field->structField.value = NULL;

    // Remove the @signal attribute
    removeAttribute(field, rtl_signal_attr);
}

// ============================================================================
// Component Transformation Entry Point
// ============================================================================

void rtlTransformComponent(MemPool *pool, RtlComponentInfo *comp, StrPool *strings)
{
    // Transform each port field
    for (RtlPortInfo *port = comp->ports; port; port = port->next) {
        transformPortField(pool, port, strings);
    }

    // Transform each signal field
    for (RtlSignalInfo *signal = comp->signals; signal; signal = signal->next) {
        transformSignalField(pool, signal, strings);
    }

    // Generate and inject wrapper functions for methods
    rtlGenerateWrappers(pool, strings, comp);

    // Generate and inject constructor
    rtlGenerateConstructor(pool, strings, comp);
}
