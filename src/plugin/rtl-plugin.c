//
// RTL Plugin - Main entry point
//
// Registers with CXY compiler and processes @rtl::component via actions.
//
// Actions registered:
//   rtl::component!(...) - marks a struct/class for component transformation
//

#include "state.h"

#include <cxy/core/hash.h>
#include <cxy/core/log.h>
#include <cxy/core/mempool.h>
#include <cxy/core/strpool.h>
#include <cxy/core/utils.h>
#include <cxy/ast.h>
#include <cxy/flag.h>
#include <cxy/plugin.h>

#include <string.h>

// Pull in implementation files
#include "discovery.c"
#include "validation.c"
#include "transform.c"
#include "codegen-wrapper.c"

// ============================================================================
// Interned attribute names
// ============================================================================

cstring rtl_input;
cstring rtl_output;
cstring rtl_inout;
cstring rtl_signal_attr;
cstring rtl_method;

// Interned type names
cstring rtl_clock;
cstring rtl_signal;
cstring rtl_bitvector;
cstring rtl_bitsignal;

// Interned wrapper names
cstring rtl_inport;
cstring rtl_outport;
cstring rtl_inoutport;
cstring rtl_inbitport;
cstring rtl_outbitport;
cstring rtl_inoutbitport;

// Interned event names
cstring rtl_posedge;
cstring rtl_negedge;
cstring rtl_valueChanged;
cstring rtl_change;

// Interned method names
cstring rtl_running;
cstring rtl_wait;
cstring rtl_waitAny;
cstring rtl_waitAll;

// Interned trigger keywords
cstring rtl_and;

// Interned type names for EventGroup
cstring rtl_EventGroup;

// Interned macro names
cstring rtl_tupleof;

// ============================================================================
// Action: rtl::component!(struct_def)
//
// Marks a struct/class definition for RTL component transformation.
// The action receives the struct/class AST node and marks it for later
// processing by adding it to the plugin's component list.
// ============================================================================

static AstNode *rtlComponentAction(CxyPluginContext *ctx,
                                   const AstNode    *node,
                                   AstNode          *args)
{
    RtlPluginState *state = (RtlPluginState *)cxyPluginState(ctx);

    // Expect exactly one argument: the struct/class definition
    CXY_REQUIRED_ARG(ctx->L, structArg, args, &node->loc);

    if (args != NULL) {
        logError(ctx->L, &args->loc,
                 "rtl::component!: too many arguments - expected single struct definition",
                 NULL);
        return NULL;
    }

    // Unwrap generic declaration to get the inner struct/class
    AstNode *innerDecl = structArg;
    if (nodeIs(structArg, GenericDecl))
        innerDecl = structArg->genericDecl.decl;

    // Validate that argument is a struct or class declaration
    if (!nodeIs(innerDecl, StructDecl) && !nodeIs(innerDecl, ClassDecl)) {
        logError(ctx->L, &structArg->loc,
                 "rtl::component!: argument must be a struct or class declaration",
                 NULL);
        return NULL;
    }

    // Discover component structure (ports, methods, etc.)
    RtlComponentInfo *comp = rtlDiscoverComponent(ctx, innerDecl, ctx->L, &node->loc);
    if (!comp) {
        return structArg;  // Error already logged
    }

    // Validate component structure
    if (!rtlValidateComponent(ctx->L, comp)) {
        return structArg;  // Validation errors logged
    }

    // Transform port field types (T → InPort[T], etc.)
    rtlTransformComponent(ctx->pool, comp, ctx->strings);

    // Add to component list
    if (!state->components) {
        state->components = comp;
    } else {
        RtlComponentInfo *last = state->components;
        while (last->next) last = last->next;
        last->next = comp;
    }

    // TODO: Validate, transform, and generate code
    // For now, return the original node unchanged
    return structArg;
}

// ============================================================================
// Action: rtl::vcd!(path, clk)
//
// Creates a Vcd trace instance.
// Transforms: rtl::vcd!("file.trace", clk) → Vcd("file.trace", clk)
// ============================================================================

static AstNode *rtlVcdAction(CxyPluginContext *ctx,
                             const AstNode *node,
                             AstNode *args)
{
    // Expect exactly two arguments: path (string) and clock
    CXY_REQUIRED_ARG(ctx->L, pathArg, args, &node->loc);
    CXY_REQUIRED_ARG(ctx->L, clkArg, args, &node->loc);

    if (clkArg->next != NULL) {
        logError(ctx->L, &clkArg->next->loc,
                 "rtl::vcd!: too many arguments - expected (path, clock)",
                 NULL);
        return NULL;
    }

    // Validate path is a string literal
    if (!nodeIs(pathArg, StringLit)) {
        logError(ctx->L, &pathArg->loc,
                 "rtl::vcd!: first argument must be a string literal",
                 NULL);
        return NULL;
    }

    // Create constructor call: Vcd(path, clk)
    AstNode *vcdPath = makeResolvedPath(ctx->pool,
                                        &node->loc,
                                        makeString(ctx->strings, "Vcd"),
                                        flgNone,
                                        NULL,  // resolveTo - will be resolved later
                                        NULL,  // next
                                        NULL); // enclosing

    AstNode *constructorCall = makeCallExpr(ctx->pool,
                                            &node->loc,
                                            vcdPath,
                                            pathArg,    // first arg: path
                                            flgNone,
                                            NULL,
                                            NULL);

    // Add clock as second argument
    pathArg->next = clkArg;
    clkArg->next = NULL;

    return constructorCall;
}

// ============================================================================
// Action: rtl::trace!(vcd, signals...)
//
// Adds signals to a VCD trace.
// Transforms: rtl::trace!(vcd, sig1, sig2) →
//             vcd.addSignal("sig1", sig1); vcd.addSignal("sig2", sig2);
// ============================================================================

static AstNode *rtlTraceAction(CxyPluginContext *ctx,
                               const AstNode *node,
                               AstNode *args)
{
    // Expect at least two arguments: vcd handle + signals
    CXY_REQUIRED_ARG(ctx->L, vcdArg, args, &node->loc);
    CXY_REQUIRED_ARG(ctx->L, firstSig, args, &node->loc);

    // Relink args
    firstSig->next = args;
    // Build a block statement with addSignal calls for each signal
    AstNodeList stmtList = {};

    for (AstNode *it = firstSig; it;) {
        AstNode *sig = it;
        it = it->next;
        sig->next = NULL;

        // Extract signal name from the expression
        cstring sigName = NULL;

        if (nodeIs(sig, Identifier)) {
            // Simple identifier or path - use the name
            sigName = sig->_name;
        } else if (nodeIs(sig, Path)) {
            FormatState state = newFormatState("", true);
            AstNode *elem = sig->path.elements;
            for (; elem; elem = elem->next) {
                format(&state, "{s}", (FormatArg[]){{.s = elem->_name}});
            }
            char *path = formatStateToString(&state);
            freeFormatState(&state);
            sigName = makeString(ctx->strings, path);
            free(path);
        } else {
            logError(ctx->L, &sig->loc,
                     "rtl::trace!: signal must be an identifier or member expression",
                     NULL);
            return NULL;
        }

        // Create string literal for signal name
        AstNode *nameStr = makeStringLiteral(ctx->pool,
                                             &sig->loc,
                                             sigName,
                                             NULL,
                                             NULL);

        // Create: vcd.addSignal("name", sig)
        AstNode *addSignalIdent = makeIdentifier(ctx->pool,
                                                 &node->loc,
                                                 makeString(ctx->strings, "addSignal"),
                                                 0,
                                                 NULL,
                                                 NULL);

        AstNode *memberAccess = makeMemberExpr(ctx->pool,
                                               &node->loc,
                                               flgNone,
                                               vcdArg,
                                               addSignalIdent,
                                               NULL,
                                               NULL);

        // Build argument list: ("name", sig)
        AstNode *callArgs = nameStr;
        nameStr->next = sig;

        AstNode *addCall = makeCallExpr(ctx->pool,
                                        &node->loc,
                                        memberAccess,
                                        callArgs,
                                        flgNone,
                                        NULL,
                                        NULL);

        // Wrap in expression statement
        AstNode *stmt = makeExprStmt(ctx->pool,
                                     &node->loc,
                                     flgNone,
                                     addCall,
                                     NULL,
                                     NULL);

        insertAstNode(&stmtList, stmt);

        // Clone arguments for next iteration
        if (sig->next) {
            sig->next = deepCloneAstNode(ctx->pool, sig->next);
        }
    }

    // Return block statement
    return makeBlockStmt(ctx->pool,
                         &node->loc,
                         stmtList.first,
                         NULL,
                         NULL);
}

// ============================================================================
// Plugin lifecycle - initialization
// ============================================================================

bool pluginInit(CxyPluginContext *ctx, const FileLoc *loc)
{
    // Intern attribute name strings
    rtl_input = makeString(ctx->strings, "input");
    rtl_output = makeString(ctx->strings, "output");
    rtl_inout = makeString(ctx->strings, "inout");
    rtl_signal_attr = makeString(ctx->strings, "signal");
    rtl_method = makeString(ctx->strings, "method");

    // Intern type name strings
    rtl_clock = makeString(ctx->strings, "Clock");
    rtl_signal = makeString(ctx->strings, "Signal");
    rtl_bitvector = makeString(ctx->strings, "BitVector");
    rtl_bitsignal = makeString(ctx->strings, "BitSignal");

    // Intern wrapper name strings
    rtl_inport = makeString(ctx->strings, "InPort");
    rtl_outport = makeString(ctx->strings, "OutPort");
    rtl_inoutport = makeString(ctx->strings, "InOutPort");
    rtl_inbitport = makeString(ctx->strings, "InBitPort");
    rtl_outbitport = makeString(ctx->strings, "OutBitPort");
    rtl_inoutbitport = makeString(ctx->strings, "InOutBitPort");

    // Intern event name strings
    rtl_posedge = makeString(ctx->strings, "posedge");
    rtl_negedge = makeString(ctx->strings, "negedge");
    rtl_valueChanged = makeString(ctx->strings, "valueChanged");
    rtl_change = makeString(ctx->strings, "change");

    // Intern method name strings
    rtl_running = makeString(ctx->strings, "running");
    rtl_wait = makeString(ctx->strings, "wait");
    rtl_waitAny = makeString(ctx->strings, "waitAny");
    rtl_waitAll = makeString(ctx->strings, "waitAll");

    // Intern trigger keyword strings
    rtl_and = makeString(ctx->strings, "_and");

    // Intern EventGroup type name
    rtl_EventGroup = makeString(ctx->strings, "EventGroup");

    // Intern macro names
    rtl_tupleof = makeString(ctx->strings, "tupleof");

    // Allocate plugin state using CXY's memory pool
    RtlPluginState *state = callocFromMemPool(ctx->pool, 1, sizeof(RtlPluginState));

    // Store references to CXY infrastructure
    state->pool = ctx->pool;
    state->strings = ctx->strings;
    state->L = ctx->L;
    state->components = NULL;  // Will be populated during discovery

    // Register state and declare this plugin operates at the parser level
    // This allows us to process actions during parsing
    cxyPluginInitialize(ctx, state, pipParser);

    // Register plugin actions
    // Users will call them as rtl::action!(...) in code
    bool success = cxyPluginRegisterAction(
        ctx,
        loc,
        (CxyPluginAction[]){
            {.name = "component", .fn = rtlComponentAction},
            {.name = "vcd", .fn = rtlVcdAction},
            {.name = "trace", .fn = rtlTraceAction},
        },
        3);

    if (!success) {
        logError(ctx->L, loc,
                 "rtl: failed to register component action",
                 NULL);
        return false;
    }

    return true;
}

// ============================================================================
// Plugin lifecycle - cleanup
// ============================================================================

void pluginDeInit(CxyPluginContext *ctx)
{
    RtlPluginState *state = (RtlPluginState *)cxyPluginState(ctx);
    if (state == NULL)
        return;

    // All allocations are from ctx->pool which CXY will free
    // No explicit cleanup needed
}
