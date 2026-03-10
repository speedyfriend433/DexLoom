#include "../Include/dx_vm.h"
#include "../Include/dx_log.h"
#include "../Include/dx_runtime.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define TAG "Interp"

#include "../Include/dx_memory.h"

// Forward declaration for get_current_dex (defined below)
static DxDexFile *get_current_dex(DxVM *vm);

// ---- ULEB128 / SLEB128 readers for try/catch parsing ----

static uint32_t interp_read_uleb128(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint32_t result = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *p++;
        result |= (uint32_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    *pp = p;
    return result;
}

static int32_t interp_read_sleb128(const uint8_t **pp) {
    const uint8_t *p = *pp;
    int32_t result = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *p++;
        result |= (int32_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    // Sign extend
    if (shift < 32 && (b & 0x40)) {
        result |= -(1 << shift);
    }
    *pp = p;
    return result;
}

// DEX try_item structure (8 bytes each, after insns array)
typedef struct {
    uint32_t start_addr;   // start of try block (code unit offset)
    uint16_t insn_count;   // length of try block (code units)
    uint16_t handler_off;  // offset to encoded_catch_handler from handler list start
} DxTryItem;

// Search try/catch handlers for a matching handler at the given pc.
// Returns the handler address (code unit offset) or UINT32_MAX if no handler found.
// If found, stores the exception object in frame->exception.
static uint32_t find_catch_handler(DxVM *vm, DxFrame *frame, const uint16_t *insns,
                                    uint32_t insns_size, uint16_t tries_size,
                                    uint32_t throw_pc, DxObject *exception) {
    if (tries_size == 0) return UINT32_MAX;

    // Compute pointer to try_items: right after insns array, padded to 4-byte boundary
    const uint8_t *insns_end = (const uint8_t *)(insns + insns_size);
    // If insns_size is odd, there's 2 bytes of padding
    if (insns_size & 1) {
        insns_end += 2;
    }

    const uint8_t *try_data = insns_end;

    // Parse try_items to find one covering throw_pc
    uint16_t matched_handler_off = 0;
    bool found_try = false;
    for (uint16_t i = 0; i < tries_size; i++) {
        // Each try_item is 8 bytes: uint32_t start_addr, uint16_t insn_count, uint16_t handler_off
        const uint8_t *entry = try_data + (i * 8);
        uint32_t start_addr = entry[0] | (entry[1] << 8) | (entry[2] << 16) | (entry[3] << 24);
        uint16_t insn_count_val = entry[4] | (entry[5] << 8);
        uint16_t handler_off = entry[6] | (entry[7] << 8);

        if (throw_pc >= start_addr && throw_pc < start_addr + insn_count_val) {
            matched_handler_off = handler_off;
            found_try = true;
            break;
        }
    }

    if (!found_try) return UINT32_MAX;

    // Handler list starts right after all try_items
    const uint8_t *handlers_base = try_data + (tries_size * 8);
    const uint8_t *handler_ptr = handlers_base + matched_handler_off;

    // Parse encoded_catch_handler
    int32_t handler_size = interp_read_sleb128(&handler_ptr);
    bool has_catch_all = (handler_size <= 0);
    int32_t abs_size = handler_size < 0 ? -handler_size : handler_size;

    // Get the exception's type descriptor for matching
    const char *exc_descriptor = NULL;
    if (exception && exception->klass) {
        exc_descriptor = exception->klass->descriptor;
    }

    // Check each typed catch handler
    for (int32_t i = 0; i < abs_size; i++) {
        uint32_t type_idx = interp_read_uleb128(&handler_ptr);
        uint32_t addr = interp_read_uleb128(&handler_ptr);

        // Try to match the exception type
        DxDexFile *dex = get_current_dex(vm);
        const char *catch_type = NULL;
        if (dex && type_idx < dex->type_count) {
            catch_type = dx_dex_get_type(dex, type_idx);
        }

        if (catch_type && exc_descriptor) {
            // Exact match or catch java/lang/Throwable or java/lang/Exception (broad catches)
            if (strcmp(catch_type, exc_descriptor) == 0 ||
                strcmp(catch_type, "Ljava/lang/Throwable;") == 0 ||
                strcmp(catch_type, "Ljava/lang/Exception;") == 0) {
                DX_DEBUG(TAG, "Exception %s caught by handler at addr %u (type %s)",
                         exc_descriptor, addr, catch_type);
                frame->exception = exception;
                return addr;
            }

            // Walk the exception's class hierarchy for a match
            if (exception && exception->klass) {
                DxClass *cls = exception->klass->super_class;
                while (cls) {
                    if (cls->descriptor && strcmp(catch_type, cls->descriptor) == 0) {
                        DX_DEBUG(TAG, "Exception %s caught by handler at addr %u (super type %s)",
                                 exc_descriptor, addr, catch_type);
                        frame->exception = exception;
                        return addr;
                    }
                    cls = cls->super_class;
                }
            }
        }
    }

    // If there's a catch-all handler, use it
    if (has_catch_all) {
        uint32_t catch_all_addr = interp_read_uleb128(&handler_ptr);
        DX_DEBUG(TAG, "Exception %s caught by catch-all handler at addr %u",
                 exc_descriptor ? exc_descriptor : "unknown", catch_all_addr);
        frame->exception = exception;
        return catch_all_addr;
    }

    return UINT32_MAX;
}

// Find a catch-all (finally) handler covering the given pc.
// Unlike find_catch_handler, this does NOT require an exception — it is used
// on normal return paths to ensure finally blocks execute.
// Returns the handler address or UINT32_MAX if no catch-all covers this pc.
static uint32_t find_finally_handler(const uint16_t *insns, uint32_t insns_size,
                                      uint16_t tries_size, uint32_t pc) {
    if (tries_size == 0) return UINT32_MAX;

    const uint8_t *insns_end = (const uint8_t *)(insns + insns_size);
    if (insns_size & 1) {
        insns_end += 2;
    }
    const uint8_t *try_data = insns_end;

    // Search all try_items covering this pc
    for (uint16_t i = 0; i < tries_size; i++) {
        const uint8_t *entry = try_data + (i * 8);
        uint32_t start_addr = entry[0] | (entry[1] << 8) | (entry[2] << 16) | (entry[3] << 24);
        uint16_t insn_count_val = entry[4] | (entry[5] << 8);
        uint16_t handler_off = entry[6] | (entry[7] << 8);

        if (pc >= start_addr && pc < start_addr + insn_count_val) {
            // Parse encoded_catch_handler to check for catch-all
            const uint8_t *handlers_base = try_data + (tries_size * 8);
            const uint8_t *handler_ptr = handlers_base + handler_off;
            int32_t handler_size = interp_read_sleb128(&handler_ptr);
            bool has_catch_all = (handler_size <= 0);
            int32_t abs_size = handler_size < 0 ? -handler_size : handler_size;

            // Skip over typed handlers
            for (int32_t j = 0; j < abs_size; j++) {
                (void)interp_read_uleb128(&handler_ptr); // type_idx
                (void)interp_read_uleb128(&handler_ptr); // addr
            }

            if (has_catch_all) {
                uint32_t catch_all_addr = interp_read_uleb128(&handler_ptr);
                // Only return it if the handler is outside the try block range
                // (to avoid looping back into the same try block endlessly)
                if (catch_all_addr < start_addr || catch_all_addr >= start_addr + insn_count_val) {
                    return catch_all_addr;
                }
            }
        }
    }
    return UINT32_MAX;
}

// Decode register arguments from 35c format (invoke-kind)
static void decode_35c_args(const uint16_t *insns, uint32_t pc,
                             uint8_t *arg_count, uint8_t args[5]) {
    uint16_t inst = insns[pc];
    uint16_t arg_word = insns[pc + 2];

    *arg_count = (inst >> 12) & 0x0F;
    args[0] = arg_word & 0x0F;
    args[1] = (arg_word >> 4) & 0x0F;
    args[2] = (arg_word >> 8) & 0x0F;
    args[3] = (arg_word >> 12) & 0x0F;
    args[4] = (inst >> 8) & 0x0F;  // for 5-arg case, vG is in the A field
}

// Pack trailing arguments into an Object[] for varargs methods.
// fixed_params is the number of declared (non-varargs) parameters (excluding 'this').
// is_static: true if the method is static (no implicit 'this' argument).
// args/argc are the original arguments; on return they are rewritten in-place.
// Returns the new argc. The packed array is allocated on the VM heap.
static uint8_t pack_varargs(DxVM *vm, DxValue *args, uint8_t argc,
                            uint32_t fixed_params, bool is_static) {
    // 'this' occupies args[0] for instance methods
    uint32_t this_offset = is_static ? 0 : 1;
    // Index in args[] where the vararg values start
    uint32_t vararg_start = this_offset + fixed_params;

    if (vararg_start > argc) {
        // Fewer args than fixed params -- nothing to pack (shouldn't happen normally)
        return argc;
    }

    uint32_t vararg_count = argc - vararg_start;

    // Allocate an Object[] array on the VM heap
    DxObject *arr = dx_vm_alloc_array(vm, vararg_count);
    if (!arr) {
        // OOM -- leave args unchanged; the callee will see raw args
        return argc;
    }

    // Copy trailing args into the array
    for (uint32_t i = 0; i < vararg_count; i++) {
        arr->array_elements[i] = args[vararg_start + i];
    }

    // Replace the trailing args with the single array argument
    args[vararg_start] = DX_OBJ_VALUE(arr);
    return (uint8_t)(vararg_start + 1);
}

// Resolve and execute an invoke instruction
static DxResult handle_invoke(DxVM *vm, DxFrame *frame, const uint16_t *code,
                                uint32_t pc, uint8_t opcode) {
    uint16_t method_idx = code[pc + 1];
    uint8_t argc;
    uint8_t arg_regs[5];
    decode_35c_args(code, pc, &argc, arg_regs);

    DxMethod *target = dx_vm_resolve_method(vm, method_idx);

    if (!target) {
        DxDexFile *cur = (frame->method && frame->method->declaring_class &&
                          frame->method->declaring_class->dex_file)
                         ? frame->method->declaring_class->dex_file : vm->dex;
        const char *cls_name = dx_dex_get_method_class(cur, method_idx);
        const char *mth_name = dx_dex_get_method_name(cur, method_idx);
        DX_WARN(TAG, "Cannot resolve method %s.%s - skipping",
                cls_name ? cls_name : "?", mth_name ? mth_name : "?");
        frame->result = DX_NULL_VALUE;
        frame->has_result = true;
        return DX_OK;
    }

    // For invoke-virtual, resolve actual target from receiver's vtable
    if (opcode == 0x6E && argc > 0 && target->vtable_idx >= 0) {
        DxValue recv_val = frame->registers[arg_regs[0]];
        if (recv_val.tag == DX_VAL_OBJ && recv_val.obj) {
            DxObject *receiver = recv_val.obj;
            if (receiver->klass &&
                (uint32_t)target->vtable_idx < receiver->klass->vtable_size) {
                DxMethod *vtable_target = receiver->klass->vtable[target->vtable_idx];
                if (vtable_target) target = vtable_target;
            }
        }
    }

    // For invoke-interface, dispatch on the receiver's actual class.
    // 1) Check if the concrete class (or its parents) overrides the method
    // 2) If not found, search implemented interfaces for a default method
    if (opcode == 0x72 && argc > 0) {
        DxValue recv_val = frame->registers[arg_regs[0]];
        if (recv_val.tag == DX_VAL_OBJ && recv_val.obj && recv_val.obj->klass) {
            DxClass *recv_cls = recv_val.obj->klass;
            // Try the receiver's class hierarchy first (concrete override)
            DxMethod *override = dx_vm_find_method(recv_cls, target->name, target->shorty);
            if (override) {
                target = override;
            } else {
                // Fall back to interface default method search
                DxMethod *iface_default = dx_vm_find_interface_method(vm, recv_cls,
                                                                        target->name, target->shorty);
                if (iface_default) target = iface_default;
            }
        }
        // If target is a bridge method, try to find the non-bridge version
        if ((target->access_flags & DX_ACC_BRIDGE) && target->declaring_class) {
            DxMethod *real = dx_vm_find_method(target->declaring_class, target->name, target->shorty);
            if (real && !(real->access_flags & DX_ACC_BRIDGE)) target = real;
        }
    }

    // For invoke-super, resolve on the declaring class's super (Dalvik semantics)
    if (opcode == 0x6F && argc > 0) {
        DxClass *declaring = target->declaring_class;
        DxMethod *original_target = target;
        bool super_found = false;
        if (declaring && declaring->super_class) {
            DxMethod *super_method = dx_vm_find_method(declaring->super_class,
                                                        target->name, target->shorty);
            if (super_method && super_method != original_target) {
                target = super_method;
                super_found = true;
            } else if (target->vtable_idx >= 0) {
                // Fallback: try vtable lookup on receiver's super
                DxValue recv_val = frame->registers[arg_regs[0]];
                if (recv_val.tag == DX_VAL_OBJ && recv_val.obj) {
                    DxObject *receiver = recv_val.obj;
                    if (receiver->klass && receiver->klass->super_class) {
                        DxClass *super = receiver->klass->super_class;
                        if ((uint32_t)target->vtable_idx < super->vtable_size) {
                            DxMethod *vtbl = super->vtable[target->vtable_idx];
                            if (vtbl && vtbl != original_target) {
                                target = vtbl;
                                super_found = true;
                            }
                        }
                    }
                }
            }
        }
        // If super method not found or resolves to self, skip to avoid infinite recursion
        if (!super_found) {
            DX_WARN(TAG, "invoke-super: no super for %s.%s - skipping",
                    original_target->declaring_class ? original_target->declaring_class->descriptor : "?",
                    original_target->name);
            frame->result = DX_NULL_VALUE;
            frame->has_result = true;
            return DX_OK;
        }
    }

    // Build argument array
    DxValue call_args[5];
    for (uint8_t i = 0; i < argc && i < 5; i++) {
        call_args[i] = frame->registers[arg_regs[i]];
    }

    // Varargs packing: if target has ACC_VARARGS, pack trailing args into Object[]
    if ((target->access_flags & DX_ACC_VARARGS) && target->shorty) {
        uint32_t fixed_params = (uint32_t)(strlen(target->shorty) - 1); // shorty[0] is return type
        if (fixed_params > 0) fixed_params--; // last declared param is the varargs array
        bool is_static = (target->access_flags & DX_ACC_STATIC) != 0;
        argc = pack_varargs(vm, call_args, argc, fixed_params, is_static);
    }

    DxValue call_result;
    memset(&call_result, 0, sizeof(call_result));
    DxResult res = dx_vm_execute_method(vm, target, call_args, argc, &call_result);

    // Store result for move-result
    frame->result = call_result;
    frame->has_result = true;

    // Exception unwinding: callee threw and didn't catch — try our catch handlers
    if (res == DX_ERR_EXCEPTION && vm->pending_exception) {
        DxObject *exc = vm->pending_exception;
        const char *exc_class = exc->klass ? exc->klass->descriptor : "unknown";
        DxMethod *caller_method = frame->method;
        if (caller_method && caller_method->code.tries_size > 0) {
            uint32_t handler_addr = find_catch_handler(vm, frame,
                caller_method->code.insns, caller_method->code.insns_size,
                caller_method->code.tries_size, pc, exc);
            if (handler_addr != UINT32_MAX) {
                DX_INFO(TAG, "Exception %s unwound to %s.%s handler at %u",
                        exc_class,
                        caller_method->declaring_class ? caller_method->declaring_class->descriptor : "?",
                        caller_method->name, handler_addr);
                vm->pending_exception = NULL;
                frame->pc = handler_addr;
                return DX_OK; // caller must jump to handler_addr
            }
        }
        // No handler here either — keep propagating
        DX_DEBUG(TAG, "Exception %s not caught in %s.%s, propagating further",
                exc_class,
                caller_method ? (caller_method->declaring_class ? caller_method->declaring_class->descriptor : "?") : "?",
                caller_method ? caller_method->name : "?");
        return DX_ERR_EXCEPTION;
    }

    // Non-fatal errors from sub-calls: log and continue execution
    if (res != DX_OK && res != DX_ERR_STACK_OVERFLOW) {
        DX_WARN(TAG, "Method %s.%s returned %s (absorbed)",
                target->declaring_class ? target->declaring_class->descriptor : "?",
                target->name, dx_result_string(res));
        return DX_OK;
    }
    return res;
}

// Resolve and execute an invoke/range instruction
static DxResult handle_invoke_range(DxVM *vm, DxFrame *frame, const uint16_t *code,
                                      uint32_t pc, uint8_t opcode) {
    uint16_t method_idx = code[pc + 1];
    uint16_t inst = code[pc];
    uint8_t argc = (inst >> 8) & 0xFF;
    uint16_t first_reg = code[pc + 2];

    DxMethod *target = dx_vm_resolve_method(vm, method_idx);

    if (!target) {
        DxDexFile *cur = (frame->method && frame->method->declaring_class &&
                          frame->method->declaring_class->dex_file)
                         ? frame->method->declaring_class->dex_file : vm->dex;
        const char *cls_name = dx_dex_get_method_class(cur, method_idx);
        const char *mth_name = dx_dex_get_method_name(cur, method_idx);
        DX_WARN(TAG, "Cannot resolve method %s.%s (range) - skipping",
                cls_name ? cls_name : "?", mth_name ? mth_name : "?");
        frame->result = DX_NULL_VALUE;
        frame->has_result = true;
        return DX_OK;
    }

    // vtable dispatch for invoke-virtual/range
    if (opcode == 0x74 && argc > 0 && target->vtable_idx >= 0) {
        DxValue recv_val = frame->registers[first_reg];
        if (recv_val.tag == DX_VAL_OBJ && recv_val.obj) {
            DxObject *receiver = recv_val.obj;
            if (receiver->klass &&
                (uint32_t)target->vtable_idx < receiver->klass->vtable_size) {
                DxMethod *vt = receiver->klass->vtable[target->vtable_idx];
                if (vt) target = vt;
            }
        }
    }

    // invoke-interface/range: dispatch on receiver's actual class + interface defaults
    if (opcode == 0x78 && argc > 0) {
        DxValue recv_val = frame->registers[first_reg];
        if (recv_val.tag == DX_VAL_OBJ && recv_val.obj && recv_val.obj->klass) {
            DxClass *recv_cls = recv_val.obj->klass;
            DxMethod *override = dx_vm_find_method(recv_cls, target->name, target->shorty);
            if (override) {
                target = override;
            } else {
                DxMethod *iface_default = dx_vm_find_interface_method(vm, recv_cls,
                                                                        target->name, target->shorty);
                if (iface_default) target = iface_default;
            }
        }
        // Bridge method unwrap
        if ((target->access_flags & DX_ACC_BRIDGE) && target->declaring_class) {
            DxMethod *real = dx_vm_find_method(target->declaring_class, target->name, target->shorty);
            if (real && !(real->access_flags & DX_ACC_BRIDGE)) target = real;
        }
    }

    // invoke-super/range: resolve on declaring class's superclass (Dalvik semantics)
    if (opcode == 0x75 && argc > 0) {
        DxClass *declaring = target->declaring_class;
        DxMethod *original_target = target;
        bool super_found = false;
        if (declaring && declaring->super_class) {
            DxMethod *super_method = dx_vm_find_method(declaring->super_class,
                                                        target->name, target->shorty);
            if (super_method && super_method != original_target) {
                target = super_method;
                super_found = true;
            } else if (target->vtable_idx >= 0) {
                DxValue recv_val = frame->registers[first_reg];
                if (recv_val.tag == DX_VAL_OBJ && recv_val.obj) {
                    DxObject *receiver = recv_val.obj;
                    if (receiver->klass && receiver->klass->super_class) {
                        DxClass *super = receiver->klass->super_class;
                        if ((uint32_t)target->vtable_idx < super->vtable_size) {
                            DxMethod *vtbl = super->vtable[target->vtable_idx];
                            if (vtbl && vtbl != original_target) {
                                target = vtbl;
                                super_found = true;
                            }
                        }
                    }
                }
            }
        }
        if (!super_found) {
            DX_WARN(TAG, "invoke-super/range: no super for %s.%s - skipping",
                    original_target->declaring_class ? original_target->declaring_class->descriptor : "?",
                    original_target->name);
            frame->result = DX_NULL_VALUE;
            frame->has_result = true;
            return DX_OK;
        }
    }

    // Build argument array on the stack (max 255 args but clamp for safety)
    uint8_t clamped_argc = argc > 16 ? 16 : argc;
    DxValue call_args[16];
    memset(call_args, 0, sizeof(call_args));
    for (uint8_t i = 0; i < clamped_argc; i++) {
        uint16_t reg = first_reg + i;
        if (reg < DX_MAX_REGISTERS) {
            call_args[i] = frame->registers[reg];
        }
    }

    // Varargs packing: if target has ACC_VARARGS, pack trailing args into Object[]
    if ((target->access_flags & DX_ACC_VARARGS) && target->shorty) {
        uint32_t fixed_params = (uint32_t)(strlen(target->shorty) - 1);
        if (fixed_params > 0) fixed_params--; // last declared param is the varargs array
        bool is_static = (target->access_flags & DX_ACC_STATIC) != 0;
        clamped_argc = pack_varargs(vm, call_args, clamped_argc, fixed_params, is_static);
    }

    DxValue call_result;
    memset(&call_result, 0, sizeof(call_result));
    DxResult res = dx_vm_execute_method(vm, target, call_args, clamped_argc, &call_result);

    frame->result = call_result;
    frame->has_result = true;

    // Exception unwinding: callee threw and didn't catch — try our catch handlers
    if (res == DX_ERR_EXCEPTION && vm->pending_exception) {
        DxObject *exc = vm->pending_exception;
        const char *exc_class = exc->klass ? exc->klass->descriptor : "unknown";
        DxMethod *caller_method = frame->method;
        if (caller_method && caller_method->code.tries_size > 0) {
            uint32_t handler_addr = find_catch_handler(vm, frame,
                caller_method->code.insns, caller_method->code.insns_size,
                caller_method->code.tries_size, pc, exc);
            if (handler_addr != UINT32_MAX) {
                DX_INFO(TAG, "Exception %s unwound to %s.%s handler at %u (range)",
                        exc_class,
                        caller_method->declaring_class ? caller_method->declaring_class->descriptor : "?",
                        caller_method->name, handler_addr);
                vm->pending_exception = NULL;
                frame->pc = handler_addr;
                return DX_OK;
            }
        }
        DX_DEBUG(TAG, "Exception %s not caught in %s.%s, propagating further (range)",
                exc_class,
                caller_method ? (caller_method->declaring_class ? caller_method->declaring_class->descriptor : "?") : "?",
                caller_method ? caller_method->name : "?");
        return DX_ERR_EXCEPTION;
    }

    if (res != DX_OK && res != DX_ERR_STACK_OVERFLOW) {
        DX_WARN(TAG, "Method %s.%s (range) returned %s (absorbed)",
                target->declaring_class ? target->declaring_class->descriptor : "?",
                target->name, dx_result_string(res));
        return DX_OK;
    }
    return res;
}

// Get the DEX file for the currently executing method
static DxDexFile *get_current_dex(DxVM *vm) {
    if (vm->current_frame && vm->current_frame->method &&
        vm->current_frame->method->declaring_class &&
        vm->current_frame->method->declaring_class->dex_file) {
        return vm->current_frame->method->declaring_class->dex_file;
    }
    return vm->dex;
}

// Find the static field index within a class's DEX-defined static fields
static int32_t find_static_field_idx(DxVM *vm, DxClass *cls, const char *fname) {
    if (!cls || !vm || !fname) return -1;

    // Framework classes with field_defs: look up by name directly
    if (cls->is_framework) {
        if (cls->field_defs && cls->static_field_count > 0) {
            for (uint32_t i = 0; i < cls->static_field_count; i++) {
                if (cls->field_defs[i].name && strcmp(cls->field_defs[i].name, fname) == 0) {
                    return (int32_t)i;
                }
            }
        }
        return -1;
    }

    // Walk class hierarchy to find the field in the declaring class
    while (cls && !cls->is_framework) {
        DxDexFile *dex = cls->dex_file ? cls->dex_file : vm->dex;
        if (dex && cls->dex_class_def_idx < dex->class_count) {
            DxDexClassData *cd = dex->class_data[cls->dex_class_def_idx];
            if (cd) {
                for (uint32_t i = 0; i < cd->static_fields_count; i++) {
                    const char *name = dx_dex_get_field_name(dex, cd->static_fields[i].field_idx);
                    if (name && strcmp(name, fname) == 0) {
                        return (int32_t)i;
                    }
                }
            }
        }
        cls = cls->super_class;
    }
    return -1;
}

// Static field get/set helpers
static DxResult handle_sget(DxVM *vm, DxFrame *frame, uint8_t dst, uint16_t field_idx, bool is_object) {
    DxDexFile *dex = get_current_dex(vm);
    const char *fname = dx_dex_get_field_name(dex, field_idx);
    const char *fclass = dx_dex_get_field_class(dex, field_idx);

    DxClass *cls = dx_vm_find_class(vm, fclass);
    if (!cls) {
        dx_vm_load_class(vm, fclass, &cls);
    }

    if (cls && cls->static_fields && cls->static_field_count > 0) {
        int32_t idx = find_static_field_idx(vm, cls, fname);
        if (idx >= 0 && (uint32_t)idx < cls->static_field_count) {
            frame->registers[dst] = cls->static_fields[idx];
            return DX_OK;
        }
    }

    // Field not found or framework class - return default
    frame->registers[dst] = is_object ? DX_NULL_VALUE : DX_INT_VALUE(0);
    return DX_OK;
}

static DxResult handle_sput(DxVM *vm, DxFrame *frame, uint8_t src, uint16_t field_idx, bool is_object) {
    DxDexFile *dex = get_current_dex(vm);
    const char *fname = dx_dex_get_field_name(dex, field_idx);
    const char *fclass = dx_dex_get_field_class(dex, field_idx);
    (void)is_object;

    DxClass *cls = dx_vm_find_class(vm, fclass);
    if (!cls) {
        dx_vm_load_class(vm, fclass, &cls);
    }

    if (cls && cls->static_fields && cls->static_field_count > 0) {
        int32_t idx = find_static_field_idx(vm, cls, fname);
        if (idx >= 0 && (uint32_t)idx < cls->static_field_count) {
            cls->static_fields[idx] = frame->registers[src];
            return DX_OK;
        }
    }

    // Field not found or framework class - silently absorb
    DX_TRACE(TAG, "sput %s.%s (absorbed)", fclass ? fclass : "?", fname ? fname : "?");
    return DX_OK;
}

DxResult dx_vm_execute_method(DxVM *vm, DxMethod *method, DxValue *args,
                               uint32_t arg_count, DxValue *result) {
    if (!vm || !method) return DX_ERR_NULL_PTR;

    // Check call depth BEFORE allocating stack frame to prevent stack overflow
    if (vm->stack_depth >= DX_MAX_STACK_DEPTH) {
        DX_ERROR(TAG, "Stack overflow at %s.%s (depth %u)",
                 method->declaring_class ? method->declaring_class->descriptor : "?",
                 method->name, vm->stack_depth);
        return DX_ERR_STACK_OVERFLOW;
    }

    // Detect infinite recursion: same method pointer appearing too many times
    {
        uint32_t recur_count = 0;
        DxFrame *f = vm->current_frame;
        while (f && recur_count <= 8) {
            if (f->method == method) recur_count++;
            f = f->caller;
        }
        if (recur_count > 8) {
            DX_WARN(TAG, "Recursion limit for %s.%s (%u occurrences) - skipping",
                     method->declaring_class ? method->declaring_class->descriptor : "?",
                     method->name, recur_count);
            if (result) *result = DX_NULL_VALUE;
            return DX_OK;
        }
    }

    DX_DEBUG(TAG, ">> %s.%s %s",
             method->declaring_class ? method->declaring_class->descriptor : "?",
             method->name, method->shorty ? method->shorty : "");

    // Handle native methods
    if (method->is_native) {
        if (!method->native_fn) {
            DX_ERROR(TAG, "Native method has no implementation: %s.%s",
                     method->declaring_class->descriptor, method->name);
            return DX_ERR_METHOD_NOT_FOUND;
        }

        DxFrame *frame = dx_vm_alloc_frame(vm);
        if (!frame) return DX_ERR_OUT_OF_MEMORY;
        frame->method = method;
        frame->caller = vm->current_frame;

        for (uint32_t i = 0; i < arg_count && i < DX_MAX_REGISTERS; i++) {
            frame->registers[i] = args[i];
        }

        vm->current_frame = frame;
        vm->stack_depth++;

        DxResult res = method->native_fn(vm, frame, args, arg_count);

        vm->stack_depth--;
        vm->current_frame = frame->caller;

        if (result && frame->has_result) {
            *result = frame->result;
        }

        DX_DEBUG(TAG, "<< %s.%s (native) -> %s",
                 method->declaring_class->descriptor, method->name,
                 dx_result_string(res));
        dx_vm_free_frame(vm, frame);
        return res;
    }

    // Bytecode interpretation — if method has no code (abstract/interface), skip gracefully
    if (!method->has_code) {
        DX_WARN(TAG, "Method has no code: %s.%s (skipping)",
                 method->declaring_class ? method->declaring_class->descriptor : "?",
                 method->name);
        if (result) *result = DX_NULL_VALUE;
        return DX_OK;
    }

    DxFrame *frame = dx_vm_alloc_frame(vm);
    if (!frame) return DX_ERR_OUT_OF_MEMORY;
    frame->method = method;
    frame->caller = vm->current_frame;

    // Place arguments in the last N registers (Dalvik convention)
    uint16_t regs = method->code.registers_size;
    uint16_t ins = method->code.ins_size;

    // Bounds check: registers_size must accommodate ins_size
    if (regs > DX_MAX_REGISTERS) regs = DX_MAX_REGISTERS;
    if (ins > regs) ins = regs;

    uint16_t first_arg_reg = regs - ins;
    for (uint32_t i = 0; i < arg_count && i < ins; i++) {
        frame->registers[first_arg_reg + i] = args[i];
    }

    vm->current_frame = frame;
    vm->stack_depth++;

    const uint16_t *code = method->code.insns;
    uint32_t code_size = method->code.insns_size;
    uint32_t pc = 0;
    DxDexFile *cur_dex = get_current_dex(vm);

    DxResult exec_result = DX_OK;
    uint32_t null_access_count = 0;  // total iget/iput on null counter (not reset)

    // Register bounds checking macro: validates register index against declared count.
    // On violation, captures diagnostic and aborts the method with VERIFICATION_FAILED.
    #define CHECK_REG(idx) do { \
        if ((uint32_t)(idx) >= regs) { \
            DX_ERROR(TAG, "Register v%u out of bounds (registers_size=%u) at pc=%u in %s.%s op=0x%02x", \
                     (uint32_t)(idx), (uint32_t)regs, pc, \
                     method->declaring_class ? method->declaring_class->descriptor : "?", \
                     method->name ? method->name : "?", opcode); \
            snprintf(vm->error_msg, sizeof(vm->error_msg), \
                     "Register v%u out of bounds (max v%u) at pc=%u in %s.%s", \
                     (uint32_t)(idx), (uint32_t)(regs - 1), pc, \
                     method->declaring_class ? method->declaring_class->descriptor : "?", \
                     method->name ? method->name : "?"); \
            exec_result = DX_ERR_VERIFICATION_FAILED; \
            goto done; \
        } \
    } while (0)

    // Instruction trace ring buffer for watchdog diagnostics
    #define INSN_TRACE_SIZE 16
    struct { uint32_t pc; uint8_t opcode; } insn_trace[INSN_TRACE_SIZE];
    uint32_t insn_trace_idx = 0;

    // Macro for safe code access with bounds check
    #define CODE_AT(off) ((pc + (off)) < code_size ? code[pc + (off)] : 0)

    while (pc < code_size) {
        next_instruction: (void)0;
        // Enforce global instruction limit to prevent runaway execution
        vm->insn_count++;
        vm->insn_total++;

        // Record instruction in trace ring buffer
        insn_trace[insn_trace_idx % INSN_TRACE_SIZE].pc = pc;
        insn_trace[insn_trace_idx % INSN_TRACE_SIZE].opcode = code[pc] & 0xFF;
        insn_trace_idx++;

        if (vm->insn_limit > 0 && vm->insn_count > vm->insn_limit) {
            const char *cls_desc = method->declaring_class ? method->declaring_class->descriptor : "?";
            const char *mth_name = method->name ? method->name : "?";

            // Build a trace of the last N instructions for debugging
            char trace_buf[512];
            int tpos = 0;
            tpos += snprintf(trace_buf + tpos, sizeof(trace_buf) - tpos,
                             "Last %d instructions before budget exhaustion:\n", INSN_TRACE_SIZE);
            uint32_t start = insn_trace_idx >= INSN_TRACE_SIZE ? insn_trace_idx - INSN_TRACE_SIZE : 0;
            for (uint32_t ti = start; ti < insn_trace_idx && tpos < (int)sizeof(trace_buf) - 60; ti++) {
                uint32_t slot = ti % INSN_TRACE_SIZE;
                tpos += snprintf(trace_buf + tpos, sizeof(trace_buf) - tpos,
                                 "  pc=%u op=0x%02x (%s)\n",
                                 insn_trace[slot].pc, insn_trace[slot].opcode,
                                 dx_opcode_name(insn_trace[slot].opcode));
            }

            DX_WARN(TAG, "Instruction budget exhausted (%llu) in %s.%s at pc=%u - probable infinite loop",
                     vm->insn_limit, cls_desc, mth_name, pc);
            DX_WARN(TAG, "%s", trace_buf);

            snprintf(vm->error_msg, sizeof(vm->error_msg),
                     "Instruction budget exhausted (%llu insns) in %s.%s at pc=%u — probable infinite loop",
                     vm->insn_limit, cls_desc, mth_name, pc);

            exec_result = DX_ERR_BUDGET_EXHAUSTED;
            if (result) *result = DX_NULL_VALUE;
            goto done;
        }

        uint16_t inst = code[pc];
        uint8_t opcode = inst & 0xFF;

        DX_TRACE(TAG, "  pc=%u op=0x%02x (%s)", pc, opcode, dx_opcode_name(opcode));

        switch (opcode) {

        case 0x00: // nop
            pc += 1;
            break;

        case 0x01: { // move vA, vB (12x)
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            CHECK_REG(dst); CHECK_REG(src);
            frame->registers[dst] = frame->registers[src];
            pc += 1;
            break;
        }

        case 0x02: { // move/from16 vAA, vBBBB (22x)
            uint8_t dst = (inst >> 8) & 0xFF;
            uint16_t src = code[pc + 1];
            if (src < DX_MAX_REGISTERS)
                frame->registers[dst] = frame->registers[src];
            pc += 2;
            break;
        }

        case 0x03: { // move/16 vAAAA, vBBBB (32x)
            uint16_t dst = code[pc + 1];
            uint16_t src = code[pc + 2];
            if (dst < DX_MAX_REGISTERS && src < DX_MAX_REGISTERS)
                frame->registers[dst] = frame->registers[src];
            pc += 3;
            break;
        }

        case 0x04: { // move-wide vA, vB (12x) - treat as move for v1
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = frame->registers[src];
            if (dst + 1 < DX_MAX_REGISTERS && src + 1 < DX_MAX_REGISTERS)
                frame->registers[dst + 1] = frame->registers[src + 1];
            pc += 1;
            break;
        }

        case 0x05: { // move-wide/from16 vAA, vBBBB (22x)
            uint8_t dst = (inst >> 8) & 0xFF;
            uint16_t src = code[pc + 1];
            if (src < DX_MAX_REGISTERS) {
                frame->registers[dst] = frame->registers[src];
                if (dst + 1 < DX_MAX_REGISTERS && src + 1 < DX_MAX_REGISTERS)
                    frame->registers[dst + 1] = frame->registers[src + 1];
            }
            pc += 2;
            break;
        }

        case 0x06: { // move-wide/16 vAAAA, vBBBB (32x)
            uint16_t dst = code[pc + 1];
            uint16_t src = code[pc + 2];
            if (dst < DX_MAX_REGISTERS && src < DX_MAX_REGISTERS) {
                frame->registers[dst] = frame->registers[src];
                if (dst + 1 < DX_MAX_REGISTERS && src + 1 < DX_MAX_REGISTERS)
                    frame->registers[dst + 1] = frame->registers[src + 1];
            }
            pc += 3;
            break;
        }

        case 0x07: { // move-object vA, vB (12x)
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            CHECK_REG(dst); CHECK_REG(src);
            frame->registers[dst] = frame->registers[src];
            pc += 1;
            break;
        }

        case 0x08: { // move-object/from16 vAA, vBBBB (22x)
            uint8_t dst = (inst >> 8) & 0xFF;
            uint16_t src = code[pc + 1];
            if (src < DX_MAX_REGISTERS)
                frame->registers[dst] = frame->registers[src];
            pc += 2;
            break;
        }

        case 0x09: { // move-object/16 vAAAA, vBBBB (32x)
            uint16_t dst = code[pc + 1];
            uint16_t src = code[pc + 2];
            if (dst < DX_MAX_REGISTERS && src < DX_MAX_REGISTERS)
                frame->registers[dst] = frame->registers[src];
            pc += 3;
            break;
        }

        case 0x0A: { // move-result vAA (11x)
            uint8_t dst = (inst >> 8) & 0xFF;
            frame->registers[dst] = frame->result;
            pc += 1;
            break;
        }

        case 0x0B: { // move-result-wide vAA (11x)
            uint8_t dst = (inst >> 8) & 0xFF;
            frame->registers[dst] = frame->result;
            pc += 1;
            break;
        }

        case 0x0C: { // move-result-object vAA (11x)
            uint8_t dst = (inst >> 8) & 0xFF;
            frame->registers[dst] = frame->result;
            pc += 1;
            break;
        }

        case 0x0D: { // move-exception vAA (11x)
            uint8_t dst = (inst >> 8) & 0xFF;
            if (frame->exception) {
                frame->registers[dst] = DX_OBJ_VALUE(frame->exception);
                frame->exception = NULL;  // clear after retrieval
            } else {
                frame->registers[dst] = DX_NULL_VALUE;
            }
            pc += 1;
            break;
        }

        case 0x0E: { // return-void
            // Check for finally (catch-all) blocks covering this return
            if (method->code.tries_size > 0) {
                uint32_t finally_addr = find_finally_handler(code, code_size,
                                                              method->code.tries_size, pc);
                if (finally_addr != UINT32_MAX) {
                    DX_DEBUG(TAG, "return-void inside try-finally, running finally at %u", finally_addr);
                    frame->exception = NULL; // no exception on normal return
                    pc = finally_addr;
                    break;
                }
            }
            goto done;
        }

        case 0x0F: { // return vAA
            uint8_t src = (inst >> 8) & 0xFF;
            frame->result = frame->registers[src];
            frame->has_result = true;
            if (result) *result = frame->registers[src];
            // Check for finally blocks covering this return
            if (method->code.tries_size > 0) {
                uint32_t finally_addr = find_finally_handler(code, code_size,
                                                              method->code.tries_size, pc);
                if (finally_addr != UINT32_MAX) {
                    DX_DEBUG(TAG, "return inside try-finally, running finally at %u", finally_addr);
                    frame->exception = NULL;
                    pc = finally_addr;
                    break;
                }
            }
            goto done;
        }

        case 0x10: { // return-wide vAA
            uint8_t src = (inst >> 8) & 0xFF;
            frame->result = frame->registers[src];
            frame->has_result = true;
            if (result) *result = frame->registers[src];
            if (method->code.tries_size > 0) {
                uint32_t finally_addr = find_finally_handler(code, code_size,
                                                              method->code.tries_size, pc);
                if (finally_addr != UINT32_MAX) {
                    DX_DEBUG(TAG, "return-wide inside try-finally, running finally at %u", finally_addr);
                    frame->exception = NULL;
                    pc = finally_addr;
                    break;
                }
            }
            goto done;
        }

        case 0x11: { // return-object vAA
            uint8_t src = (inst >> 8) & 0xFF;
            frame->result = frame->registers[src];
            frame->has_result = true;
            if (result) *result = frame->registers[src];
            if (method->code.tries_size > 0) {
                uint32_t finally_addr = find_finally_handler(code, code_size,
                                                              method->code.tries_size, pc);
                if (finally_addr != UINT32_MAX) {
                    DX_DEBUG(TAG, "return-object inside try-finally, running finally at %u", finally_addr);
                    frame->exception = NULL;
                    pc = finally_addr;
                    break;
                }
            }
            goto done;
        }

        case 0x12: { // const/4 vA, #+B (11n)
            uint8_t dst = (inst >> 8) & 0x0F;
            int32_t val = (int32_t)(inst >> 12);
            if (val & 0x8) val |= (int32_t)0xFFFFFFF0;
            frame->registers[dst] = DX_INT_VALUE(val);
            pc += 1;
            break;
        }

        case 0x13: { // const/16 vAA, #+BBBB (21s)
            uint8_t dst = (inst >> 8) & 0xFF;
            int16_t val = (int16_t)code[pc + 1];
            frame->registers[dst] = DX_INT_VALUE((int32_t)val);
            pc += 2;
            break;
        }

        case 0x14: { // const vAA, #+BBBBBBBB (31i)
            uint8_t dst = (inst >> 8) & 0xFF;
            int32_t val = (int32_t)(code[pc + 1] | ((uint32_t)code[pc + 2] << 16));
            frame->registers[dst] = DX_INT_VALUE(val);
            pc += 3;
            break;
        }

        case 0x15: { // const/high16 vAA, #+BBBB0000 (21h)
            uint8_t dst = (inst >> 8) & 0xFF;
            int32_t val = (int32_t)((uint32_t)code[pc + 1] << 16);
            frame->registers[dst] = DX_INT_VALUE(val);
            pc += 2;
            break;
        }

        case 0x16: { // const-wide/16 vAA, #+BBBB (21s)
            uint8_t dst = (inst >> 8) & 0xFF;
            int64_t val = (int16_t)code[pc + 1];
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = val;
            pc += 2;
            break;
        }

        case 0x17: { // const-wide/32 vAA, #+BBBBBBBB (31i)
            uint8_t dst = (inst >> 8) & 0xFF;
            int32_t val = (int32_t)(code[pc + 1] | ((uint32_t)code[pc + 2] << 16));
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = (int64_t)val;
            pc += 3;
            break;
        }

        case 0x18: { // const-wide vAA, #+BBBBBBBBBBBBBBBB (51l)
            uint8_t dst = (inst >> 8) & 0xFF;
            int64_t val = (int64_t)code[pc + 1] |
                          ((int64_t)code[pc + 2] << 16) |
                          ((int64_t)code[pc + 3] << 32) |
                          ((int64_t)code[pc + 4] << 48);
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = val;
            pc += 5;
            break;
        }

        case 0x19: { // const-wide/high16 vAA, #+BBBB000000000000 (21h)
            uint8_t dst = (inst >> 8) & 0xFF;
            int64_t val = (int64_t)((uint64_t)code[pc + 1] << 48);
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = val;
            pc += 2;
            break;
        }

        case 0x1A: { // const-string vAA, string@BBBB (21c)
            uint8_t dst = (inst >> 8) & 0xFF;
            uint16_t str_idx = code[pc + 1];
            const char *str = dx_dex_get_string(cur_dex, str_idx);
            DxObject *str_obj = dx_vm_create_string(vm, str ? str : "");
            frame->registers[dst] = DX_OBJ_VALUE(str_obj);
            pc += 2;
            break;
        }

        case 0x1B: { // const-string/jumbo vAA, string@BBBBBBBB (31c)
            uint8_t dst = (inst >> 8) & 0xFF;
            uint32_t str_idx = code[pc + 1] | ((uint32_t)code[pc + 2] << 16);
            const char *str = dx_dex_get_string(cur_dex, str_idx);
            DxObject *str_obj = dx_vm_create_string(vm, str ? str : "");
            frame->registers[dst] = DX_OBJ_VALUE(str_obj);
            pc += 3;
            break;
        }

        case 0x1C: { // const-class vAA, type@BBBB (21c)
            uint8_t dst = (inst >> 8) & 0xFF;
            // Return null for class objects - proper Class<T> not modeled
            frame->registers[dst] = DX_NULL_VALUE;
            pc += 2;
            break;
        }

        case 0x1D: // monitor-enter (11x) - no threading model
            pc += 1;
            break;

        case 0x1E: // monitor-exit (11x) - no threading model
            pc += 1;
            break;

        case 0x1F: { // check-cast vAA, type@BBBB (21c)
            uint8_t src = (inst >> 8) & 0xFF;
            uint16_t type_idx = code[pc + 1];
            DxObject *obj = (frame->registers[src].tag == DX_VAL_OBJ) ? frame->registers[src].obj : NULL;
            if (obj) {
                const char *type_desc = dx_dex_get_type(cur_dex, type_idx);
                DxClass *cls = obj->klass;
                bool match = false;
                while (cls && !match) {
                    if (cls->descriptor && type_desc && strcmp(cls->descriptor, type_desc) == 0) {
                        match = true;
                    }
                    // Check interfaces
                    if (!match) {
                        for (uint32_t ii = 0; ii < cls->interface_count && !match; ii++) {
                            if (cls->interfaces[ii] && type_desc &&
                                strcmp(cls->interfaces[ii], type_desc) == 0) {
                                match = true;
                            }
                        }
                    }
                    cls = cls->super_class;
                }
                if (!match) {
                    // ClassCastException
                    char msg[256];
                    snprintf(msg, sizeof(msg), "%s cannot be cast to %s",
                             obj->klass->descriptor ? obj->klass->descriptor : "?",
                             type_desc ? type_desc : "?");
                    DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ClassCastException;", msg);
                    if (exc && method->code.tries_size > 0) {
                        uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                               method->code.tries_size, pc, exc);
                        if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                    }
                    // No handler - propagate
                    if (exc) {
                        vm->pending_exception = exc;
                        exec_result = DX_ERR_EXCEPTION;
                        goto done;
                    }
                    DX_WARN(TAG, "ClassCastException: %s", msg);
                }
            }
            // null passes check-cast (Java spec)
            pc += 2;
            break;
        }

        case 0x20: { // instance-of vA, vB, type@CCCC (22c)
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t obj_reg = (inst >> 12) & 0x0F;
            uint16_t type_idx = code[pc + 1];

            DxObject *obj = (frame->registers[obj_reg].tag == DX_VAL_OBJ)
                            ? frame->registers[obj_reg].obj : NULL;
            if (!obj) {
                frame->registers[dst] = DX_INT_VALUE(0);
            } else {
                const char *type_desc = dx_dex_get_type(cur_dex, type_idx);
                // Walk class hierarchy to check class and interfaces
                DxClass *cls = obj->klass;
                bool match = false;
                while (cls && !match) {
                    if (cls->descriptor && type_desc && strcmp(cls->descriptor, type_desc) == 0) {
                        match = true;
                        break;
                    }
                    // Check interfaces implemented by this class in the hierarchy
                    for (uint32_t ifc = 0; ifc < cls->interface_count && !match; ifc++) {
                        if (cls->interfaces[ifc] && type_desc &&
                            strcmp(cls->interfaces[ifc], type_desc) == 0) {
                            match = true;
                        }
                    }
                    cls = cls->super_class;
                }
                frame->registers[dst] = DX_INT_VALUE(match ? 1 : 0);
            }
            pc += 2;
            break;
        }

        case 0x21: { // array-length vA, vB (12x)
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            DxObject *arr = frame->registers[src].obj;
            if (arr && arr->is_array) {
                frame->registers[dst] = DX_INT_VALUE((int32_t)arr->array_length);
            } else {
                frame->registers[dst] = DX_INT_VALUE(0);
            }
            pc += 1;
            break;
        }

        case 0x22: { // new-instance vAA, type@BBBB (21c)
            uint8_t dst = (inst >> 8) & 0xFF;
            uint16_t type_idx = code[pc + 1];
            const char *type_desc = dx_dex_get_type(cur_dex, type_idx);

            DxClass *cls = NULL;
            exec_result = dx_vm_load_class(vm, type_desc, &cls);
            if (exec_result != DX_OK) {
                DX_WARN(TAG, "new-instance: cannot load %s, using Object", type_desc);
                cls = vm->class_object;
                exec_result = DX_OK;
            }

            exec_result = dx_vm_init_class(vm, cls);
            if (exec_result != DX_OK) goto done;

            DxObject *obj = dx_vm_alloc_object(vm, cls);
            if (!obj) { exec_result = DX_ERR_OUT_OF_MEMORY; goto done; }

            frame->registers[dst] = DX_OBJ_VALUE(obj);
            pc += 2;
            break;
        }

        case 0x23: { // new-array vA, vB, type@CCCC (22c)
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t size_reg = (inst >> 12) & 0x0F;
            int32_t length = frame->registers[size_reg].i;
            if (length < 0) length = 0;
            DxObject *arr = dx_vm_alloc_array(vm, (uint32_t)length);
            if (arr) {
                frame->registers[dst] = DX_OBJ_VALUE(arr);
            } else {
                frame->registers[dst] = DX_NULL_VALUE;
            }
            pc += 2;
            break;
        }

        case 0x24: { // filled-new-array {vC, vD, vE, vF, vG}, type@BBBB (35c)
            uint8_t argc;
            uint8_t arg_regs[5];
            decode_35c_args(code, pc, &argc, arg_regs);
            DxObject *arr = dx_vm_alloc_array(vm, argc);
            if (arr) {
                for (uint8_t i = 0; i < argc; i++) {
                    arr->array_elements[i] = frame->registers[arg_regs[i]];
                }
                frame->result = DX_OBJ_VALUE(arr);
            } else {
                frame->result = DX_NULL_VALUE;
            }
            frame->has_result = true;
            pc += 3;
            break;
        }

        case 0x25: { // filled-new-array/range {vCCCC .. vNNNN}, type@BBBB (3rc)
            uint8_t argc = (inst >> 8) & 0xFF;
            uint16_t first_reg = code[pc + 2];
            DxObject *arr = dx_vm_alloc_array(vm, argc);
            if (arr) {
                for (uint8_t i = 0; i < argc; i++) {
                    uint16_t reg = first_reg + i;
                    if (reg < DX_MAX_REGISTERS) {
                        arr->array_elements[i] = frame->registers[reg];
                    }
                }
                frame->result = DX_OBJ_VALUE(arr);
            } else {
                frame->result = DX_NULL_VALUE;
            }
            frame->has_result = true;
            pc += 3;
            break;
        }

        case 0x26: { // fill-array-data vAA, +BBBBBBBB (31t)
            uint8_t arr_reg = (inst >> 8) & 0xFF;
            int32_t offset = (int32_t)(code[pc + 1] | ((uint32_t)code[pc + 2] << 16));
            const uint16_t *payload = &code[pc + offset];
            uint16_t ident = payload[0];
            if (ident != 0x0300) {
                DX_WARN(TAG, "fill-array-data: bad ident 0x%04x at pc=%u", ident, pc);
                pc += 3;
                break;
            }
            uint16_t elem_width = payload[1];
            uint32_t size = (uint32_t)payload[2] | ((uint32_t)payload[3] << 16);
            const uint8_t *data = (const uint8_t *)&payload[4];

            DxObject *arr = frame->registers[arr_reg].obj;
            if (arr && arr->is_array && arr->array_elements) {
                uint32_t count = size < arr->array_length ? size : arr->array_length;
                for (uint32_t i = 0; i < count; i++) {
                    int32_t val = 0;
                    const uint8_t *elem = data + (i * elem_width);
                    if (elem_width == 1) {
                        val = (int8_t)elem[0];
                    } else if (elem_width == 2) {
                        val = (int16_t)(elem[0] | (elem[1] << 8));
                    } else if (elem_width == 4) {
                        val = (int32_t)(elem[0] | (elem[1] << 8) | (elem[2] << 16) | (elem[3] << 24));
                    }
                    if (elem_width == 8) {
                        // 8-byte elements (long/double)
                        int64_t val64 = 0;
                        for (int b = 0; b < 8; b++) {
                            val64 |= (int64_t)elem[b] << (b * 8);
                        }
                        arr->array_elements[i].tag = DX_VAL_LONG;
                        arr->array_elements[i].l = val64;
                    } else {
                        arr->array_elements[i] = DX_INT_VALUE(val);
                    }
                }
            }
            pc += 3;
            break;
        }

        case 0x27: { // throw vAA (11x)
            uint8_t src = (inst >> 8) & 0xFF;
            DxObject *exc = (frame->registers[src].tag == DX_VAL_OBJ) ? frame->registers[src].obj : NULL;
            const char *exc_class = exc && exc->klass ? exc->klass->descriptor : "unknown";

            // Search for a try/catch handler covering this pc
            if (method->code.tries_size > 0) {
                uint32_t handler_addr = find_catch_handler(vm, frame, code, code_size,
                                                            method->code.tries_size, pc, exc);
                if (handler_addr != UINT32_MAX) {
                    DX_INFO(TAG, "throw %s -> caught at handler addr %u", exc_class, handler_addr);
                    pc = handler_addr;
                    break;
                }
            }

            // No handler found - propagate up to caller via exception unwinding
            DX_INFO(TAG, "throw %s (no handler, propagating to caller)", exc_class);
            vm->pending_exception = exc;
            exec_result = DX_ERR_EXCEPTION;
            goto done;
        }

        case 0x28: { // goto +AA (10t)
            int8_t offset = (int8_t)((inst >> 8) & 0xFF);
            pc += offset;
            break;
        }

        case 0x29: { // goto/16 +AAAA (20t)
            int16_t offset = (int16_t)code[pc + 1];
            pc += offset;
            break;
        }

        case 0x2A: { // goto/32 +AAAAAAAA (30t)
            int32_t offset = (int32_t)(code[pc + 1] | ((uint32_t)code[pc + 2] << 16));
            pc += offset;
            break;
        }

        case 0x2B: { // packed-switch vAA, +BBBBBBBB (31t)
            uint8_t test_reg = (inst >> 8) & 0xFF;
            int32_t offset = (int32_t)(code[pc + 1] | ((uint32_t)code[pc + 2] << 16));
            uint32_t payload_pc = pc + offset;
            if (payload_pc >= code_size) {
                DX_WARN(TAG, "packed-switch: payload offset %u out of bounds (code_size=%u)", payload_pc, code_size);
                pc += 3;
                break;
            }
            const uint16_t *payload = &code[payload_pc];
            uint16_t ident = payload[0];
            if (ident != 0x0100) {
                DX_WARN(TAG, "packed-switch: bad ident 0x%04x at pc=%u", ident, pc);
                pc += 3;
                break;
            }
            uint16_t size = payload[1];
            int32_t first_key = (int32_t)(payload[2] | ((uint32_t)payload[3] << 16));
            const int32_t *targets = (const int32_t *)&payload[4];

            int32_t test_val = frame->registers[test_reg].i;
            int32_t idx = test_val - first_key;
            if (idx >= 0 && (uint32_t)idx < size) {
                pc += targets[idx];
            } else {
                pc += 3; // fall through
            }
            break;
        }

        case 0x2C: { // sparse-switch vAA, +BBBBBBBB (31t)
            uint8_t test_reg = (inst >> 8) & 0xFF;
            int32_t offset = (int32_t)(code[pc + 1] | ((uint32_t)code[pc + 2] << 16));
            uint32_t payload_pc = pc + offset;
            if (payload_pc >= code_size) {
                DX_WARN(TAG, "sparse-switch: payload offset %u out of bounds (code_size=%u)", payload_pc, code_size);
                pc += 3;
                break;
            }
            const uint16_t *payload = &code[payload_pc];
            uint16_t ident = payload[0];
            if (ident != 0x0200) {
                DX_WARN(TAG, "sparse-switch: bad ident 0x%04x at pc=%u", ident, pc);
                pc += 3;
                break;
            }
            uint16_t size = payload[1];
            const int32_t *keys = (const int32_t *)&payload[2];
            const int32_t *targets = (const int32_t *)&payload[2 + size * 2]; // after keys array

            int32_t test_val = frame->registers[test_reg].i;
            bool found = false;
            for (uint16_t si = 0; si < size; si++) {
                if (keys[si] == test_val) {
                    pc += targets[si];
                    found = true;
                    break;
                }
            }
            if (!found) {
                pc += 3; // fall through
            }
            break;
        }

        // Comparison operations (23x)
        case 0x2D: { // cmpl-float vAA, vBB, vCC
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t b = code[pc + 1] & 0xFF;
            uint8_t c = (code[pc + 1] >> 8) & 0xFF;
            float fb = frame->registers[b].f;
            float fc = frame->registers[c].f;
            frame->registers[dst] = DX_INT_VALUE(fb < fc ? -1 : (fb > fc ? 1 : 0));
            pc += 2;
            break;
        }
        case 0x2E: { // cmpg-float
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t b = code[pc + 1] & 0xFF;
            uint8_t c = (code[pc + 1] >> 8) & 0xFF;
            float fb = frame->registers[b].f;
            float fc = frame->registers[c].f;
            frame->registers[dst] = DX_INT_VALUE(fb > fc ? 1 : (fb < fc ? -1 : 0));
            pc += 2;
            break;
        }
        case 0x2F: { // cmpl-double
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t b = code[pc + 1] & 0xFF;
            uint8_t c = (code[pc + 1] >> 8) & 0xFF;
            double db = frame->registers[b].d;
            double dc = frame->registers[c].d;
            frame->registers[dst] = DX_INT_VALUE(db < dc ? -1 : (db > dc ? 1 : 0));
            pc += 2;
            break;
        }
        case 0x30: { // cmpg-double
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t b = code[pc + 1] & 0xFF;
            uint8_t c = (code[pc + 1] >> 8) & 0xFF;
            double db = frame->registers[b].d;
            double dc = frame->registers[c].d;
            frame->registers[dst] = DX_INT_VALUE(db > dc ? 1 : (db < dc ? -1 : 0));
            pc += 2;
            break;
        }
        case 0x31: { // cmp-long
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t b = code[pc + 1] & 0xFF;
            uint8_t c = (code[pc + 1] >> 8) & 0xFF;
            int64_t lb = frame->registers[b].l;
            int64_t lc = frame->registers[c].l;
            frame->registers[dst] = DX_INT_VALUE(lb < lc ? -1 : (lb > lc ? 1 : 0));
            pc += 2;
            break;
        }

        // if-test vA, vB, +CCCC (22t)
        case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
            uint8_t a = (inst >> 8) & 0x0F;
            uint8_t b = (inst >> 12) & 0x0F;
            int16_t offset = (int16_t)code[pc + 1];
            int32_t va = frame->registers[a].i;
            int32_t vb = frame->registers[b].i;
            bool take = false;
            switch (opcode) {
                case 0x32: take = (va == vb); break; // if-eq
                case 0x33: take = (va != vb); break; // if-ne
                case 0x34: take = (va <  vb); break; // if-lt
                case 0x35: take = (va >= vb); break; // if-ge
                case 0x36: take = (va >  vb); break; // if-gt
                case 0x37: take = (va <= vb); break; // if-le
            }
            pc += take ? offset : 2;
            break;
        }

        // if-testz vAA, +BBBB (21t)
        case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: {
            uint8_t src = (inst >> 8) & 0xFF;
            int16_t offset = (int16_t)code[pc + 1];
            int32_t val;
            if (frame->registers[src].tag == DX_VAL_OBJ) {
                val = (frame->registers[src].obj == NULL) ? 0 : 1;
            } else {
                val = frame->registers[src].i;
            }
            bool take = false;
            switch (opcode) {
                case 0x38: take = (val == 0); break; // if-eqz
                case 0x39: take = (val != 0); break; // if-nez
                case 0x3A: take = (val <  0); break; // if-ltz
                case 0x3B: take = (val >= 0); break; // if-gez
                case 0x3C: take = (val >  0); break; // if-gtz
                case 0x3D: take = (val <= 0); break; // if-lez
            }
            pc += take ? offset : 2;
            break;
        }

        // aget variants (23x): aget vAA, vBB, vCC
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: {
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t arr_reg = code[pc + 1] & 0xFF;
            uint8_t idx_reg = (code[pc + 1] >> 8) & 0xFF;
            CHECK_REG(dst); CHECK_REG(arr_reg); CHECK_REG(idx_reg);
            DxObject *arr = (frame->registers[arr_reg].tag == DX_VAL_OBJ) ? frame->registers[arr_reg].obj : NULL;
            int32_t index = frame->registers[idx_reg].i;
            if (!arr) {
                // NullPointerException on null array
                DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/NullPointerException;",
                    "Attempt to get array element from null array");
                if (exc && method->code.tries_size > 0) {
                    uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                           method->code.tries_size, pc, exc);
                    if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                }
                if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                frame->registers[dst] = (opcode == 0x46) ? DX_NULL_VALUE : DX_INT_VALUE(0);
            } else if (arr->is_array && index >= 0 && (uint32_t)index < arr->array_length) {
                frame->registers[dst] = arr->array_elements[index];
            } else if (arr->is_array) {
                // ArrayIndexOutOfBoundsException
                char msg[64];
                snprintf(msg, sizeof(msg), "length=%u; index=%d", arr->array_length, index);
                DxObject *exc = dx_vm_create_exception(vm,
                    "Ljava/lang/ArrayIndexOutOfBoundsException;", msg);
                if (exc && method->code.tries_size > 0) {
                    uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                           method->code.tries_size, pc, exc);
                    if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                }
                if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                frame->registers[dst] = (opcode == 0x46) ? DX_NULL_VALUE : DX_INT_VALUE(0);
            } else {
                frame->registers[dst] = (opcode == 0x46) ? DX_NULL_VALUE : DX_INT_VALUE(0);
            }
            pc += 2;
            break;
        }
        // aput variants (23x): aput vAA, vBB, vCC
        case 0x4B: case 0x4C: case 0x4D: case 0x4E:
        case 0x4F: case 0x50: case 0x51: {
            uint8_t src = (inst >> 8) & 0xFF;
            uint8_t arr_reg = code[pc + 1] & 0xFF;
            uint8_t idx_reg = (code[pc + 1] >> 8) & 0xFF;
            CHECK_REG(src); CHECK_REG(arr_reg); CHECK_REG(idx_reg);
            DxObject *arr = (frame->registers[arr_reg].tag == DX_VAL_OBJ) ? frame->registers[arr_reg].obj : NULL;
            int32_t index = frame->registers[idx_reg].i;
            if (!arr) {
                DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/NullPointerException;",
                    "Attempt to store to null array");
                if (exc && method->code.tries_size > 0) {
                    uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                           method->code.tries_size, pc, exc);
                    if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                }
                if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
            } else if (arr->is_array && index >= 0 && (uint32_t)index < arr->array_length) {
                arr->array_elements[index] = frame->registers[src];
            } else if (arr->is_array) {
                char msg[64];
                snprintf(msg, sizeof(msg), "length=%u; index=%d", arr->array_length, index);
                DxObject *exc = dx_vm_create_exception(vm,
                    "Ljava/lang/ArrayIndexOutOfBoundsException;", msg);
                if (exc && method->code.tries_size > 0) {
                    uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                           method->code.tries_size, pc, exc);
                    if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                }
                if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
            }
            pc += 2;
            break;
        }

        // iget family (22c)
        case 0x52: case 0x53: case 0x54: case 0x55:
        case 0x56: case 0x57: case 0x58: {
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t obj_reg = (inst >> 12) & 0x0F;
            uint16_t field_idx = code[pc + 1];
            CHECK_REG(dst); CHECK_REG(obj_reg);

            DxValue obj_val = frame->registers[obj_reg];
            DxObject *obj = (obj_val.tag == DX_VAL_OBJ) ? obj_val.obj : NULL;
            if (!obj) {
                null_access_count++;
                // Try to throw NullPointerException
                DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/NullPointerException;",
                    "Attempt to read from field on a null object reference");
                if (exc && method->code.tries_size > 0) {
                    uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                           method->code.tries_size, pc, exc);
                    if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                }
                if (exc && null_access_count <= 1) {
                    // First null access with no local handler — propagate
                    vm->pending_exception = exc;
                    exec_result = DX_ERR_EXCEPTION;
                    goto done;
                }
                // Fallback: absorb after first to avoid cascading failures
                if (null_access_count <= 3) {
                    DX_WARN(TAG, "iget on null object at pc=%u (NPE absorbed)", pc);
                }
                if (null_access_count > 100) {
                    DX_ERROR(TAG, "Too many null field accesses (%u), aborting method %s.%s",
                             null_access_count,
                             method->declaring_class ? method->declaring_class->descriptor : "?",
                             method->name);
                    exec_result = DX_ERR_INTERNAL;
                    goto done;
                }
                frame->registers[dst] = (opcode == 0x54) ? DX_NULL_VALUE : DX_INT_VALUE(0);
                pc += 2;
                break;
            }

            const char *fname = dx_dex_get_field_name(cur_dex, field_idx);
            DxValue val;
            DxResult fr = dx_vm_get_field(obj, fname, &val);
            if (fr == DX_OK) {
                frame->registers[dst] = val;
            } else {
                frame->registers[dst] = (opcode == 0x54) ? DX_NULL_VALUE : DX_INT_VALUE(0);
            }
            pc += 2;
            break;
        }

        // iput family (22c)
        case 0x59: case 0x5A: case 0x5B: case 0x5C:
        case 0x5D: case 0x5E: case 0x5F: {
            uint8_t src = (inst >> 8) & 0x0F;
            uint8_t obj_reg = (inst >> 12) & 0x0F;
            uint16_t field_idx = code[pc + 1];
            CHECK_REG(src); CHECK_REG(obj_reg);

            DxValue obj_val = frame->registers[obj_reg];
            DxObject *obj = (obj_val.tag == DX_VAL_OBJ) ? obj_val.obj : NULL;
            if (!obj) {
                null_access_count++;
                if (null_access_count <= 3) {
                    DX_WARN(TAG, "iput on null object at pc=%u", pc);
                }
                if (null_access_count > 100) {
                    DX_ERROR(TAG, "Too many null field accesses (%u), aborting method %s.%s",
                             null_access_count,
                             method->declaring_class ? method->declaring_class->descriptor : "?",
                             method->name);
                    exec_result = DX_ERR_INTERNAL;
                    goto done;
                }
                pc += 2;
                break;
            }

            const char *fname = dx_dex_get_field_name(cur_dex, field_idx);
            dx_vm_set_field(obj, fname, frame->registers[src]);
            pc += 2;
            break;
        }

        // sget family (21c)
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: {
            uint8_t dst = (inst >> 8) & 0xFF;
            uint16_t field_idx = code[pc + 1];
            bool is_obj = (opcode == 0x62);
            handle_sget(vm, frame, dst, field_idx, is_obj);
            pc += 2;
            break;
        }

        // sput family (21c)
        case 0x67: case 0x68: case 0x69: case 0x6A:
        case 0x6B: case 0x6C: case 0x6D: {
            uint8_t src = (inst >> 8) & 0xFF;
            uint16_t field_idx = code[pc + 1];
            bool is_obj = (opcode == 0x69);
            handle_sput(vm, frame, src, field_idx, is_obj);
            pc += 2;
            break;
        }

        // invoke-kind (35c)
        case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: {
            frame->pc = 0; // sentinel
            exec_result = handle_invoke(vm, frame, code, pc, opcode);
            if (exec_result != DX_OK) goto done;
            if (frame->pc != 0) {
                // Exception was caught by our try/catch — jump to handler
                pc = frame->pc;
                frame->pc = 0;
            } else {
                pc += 3;
            }
            break;
        }

        // invoke-kind/range (3rc)
        case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
            frame->pc = 0; // sentinel
            exec_result = handle_invoke_range(vm, frame, code, pc, opcode);
            if (exec_result != DX_OK) goto done;
            if (frame->pc != 0) {
                pc = frame->pc;
                frame->pc = 0;
            } else {
                pc += 3;
            }
            break;
        }

        // Unary operations (12x)
        case 0x7B: { // neg-int vA, vB
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE(-frame->registers[src].i);
            pc += 1;
            break;
        }
        case 0x7C: { // not-int vA, vB
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE(~frame->registers[src].i);
            pc += 1;
            break;
        }
        case 0x7D: { // neg-long
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = -frame->registers[src].l;
            pc += 1;
            break;
        }
        case 0x7E: { // not-long
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = ~frame->registers[src].l;
            pc += 1;
            break;
        }
        case 0x7F: { // neg-float
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_FLOAT;
            frame->registers[dst].f = -frame->registers[src].f;
            pc += 1;
            break;
        }
        case 0x80: { // neg-double
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_DOUBLE;
            frame->registers[dst].d = -frame->registers[src].d;
            pc += 1;
            break;
        }
        case 0x81: { // int-to-long
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = (int64_t)frame->registers[src].i;
            pc += 1;
            break;
        }
        case 0x82: { // int-to-float
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_FLOAT;
            frame->registers[dst].f = (float)frame->registers[src].i;
            pc += 1;
            break;
        }
        case 0x83: { // int-to-double
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_DOUBLE;
            frame->registers[dst].d = (double)frame->registers[src].i;
            pc += 1;
            break;
        }
        case 0x84: { // long-to-int
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE((int32_t)frame->registers[src].l);
            pc += 1;
            break;
        }
        case 0x85: { // long-to-float
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_FLOAT;
            frame->registers[dst].f = (float)frame->registers[src].l;
            pc += 1;
            break;
        }
        case 0x86: { // long-to-double
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_DOUBLE;
            frame->registers[dst].d = (double)frame->registers[src].l;
            pc += 1;
            break;
        }
        case 0x87: { // float-to-int
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE((int32_t)frame->registers[src].f);
            pc += 1;
            break;
        }
        case 0x88: { // float-to-long
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = (int64_t)frame->registers[src].f;
            pc += 1;
            break;
        }
        case 0x89: { // float-to-double
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_DOUBLE;
            frame->registers[dst].d = (double)frame->registers[src].f;
            pc += 1;
            break;
        }
        case 0x8A: { // double-to-int
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE((int32_t)frame->registers[src].d);
            pc += 1;
            break;
        }
        case 0x8B: { // double-to-long
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_LONG;
            frame->registers[dst].l = (int64_t)frame->registers[src].d;
            pc += 1;
            break;
        }
        case 0x8C: { // double-to-float
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst].tag = DX_VAL_FLOAT;
            frame->registers[dst].f = (float)frame->registers[src].d;
            pc += 1;
            break;
        }
        case 0x8D: { // int-to-byte
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE((int32_t)(int8_t)(frame->registers[src].i & 0xFF));
            pc += 1;
            break;
        }
        case 0x8E: { // int-to-char
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE((int32_t)(uint16_t)(frame->registers[src].i & 0xFFFF));
            pc += 1;
            break;
        }
        case 0x8F: { // int-to-short
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            frame->registers[dst] = DX_INT_VALUE((int32_t)(int16_t)(frame->registers[src].i & 0xFFFF));
            pc += 1;
            break;
        }

        // Binary operations (23x): binop vAA, vBB, vCC
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t b = code[pc + 1] & 0xFF;
            uint8_t c = (code[pc + 1] >> 8) & 0xFF;
            int32_t vb = frame->registers[b].i;
            int32_t vc = frame->registers[c].i;
            int32_t r = 0;
            bool use_int = true;
            switch (opcode) {
                case 0x90: r = vb + vc; break; // add-int
                case 0x91: r = vb - vc; break; // sub-int
                case 0x92: r = vb * vc; break; // mul-int
                case 0x93: // div-int
                    if (vc == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        DX_WARN(TAG, "ArithmeticException: divide by zero"); goto done;
                    }
                    // Java: INT_MIN / -1 = INT_MIN (wraps)
                    r = (vb == INT32_MIN && vc == -1) ? INT32_MIN : vb / vc; break;
                case 0x94: // rem-int
                    if (vc == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        DX_WARN(TAG, "ArithmeticException: divide by zero"); goto done;
                    }
                    // Java: INT_MIN % -1 = 0
                    r = (vb == INT32_MIN && vc == -1) ? 0 : vb % vc; break;
                case 0x95: r = vb & vc; break; // and-int
                case 0x96: r = vb | vc; break; // or-int
                case 0x97: r = vb ^ vc; break; // xor-int
                case 0x98: r = vb << (vc & 0x1F); break; // shl-int
                case 0x99: r = vb >> (vc & 0x1F); break; // shr-int
                case 0x9A: r = (int32_t)((uint32_t)vb >> (vc & 0x1F)); break; // ushr-int
                // Long operations
                case 0x9B: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb + lc; use_int = false; break; }
                case 0x9C: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb - lc; use_int = false; break; }
                case 0x9D: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb * lc; use_int = false; break; }
                case 0x9E: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    if (lc == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        goto done;
                    }
                    // LLONG_MIN / -1 = LLONG_MIN in Java
                    frame->registers[dst].tag = DX_VAL_LONG;
                    frame->registers[dst].l = (lb == INT64_MIN && lc == -1) ? INT64_MIN : lb / lc;
                    use_int = false; break; }
                case 0x9F: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    if (lc == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        goto done;
                    }
                    frame->registers[dst].tag = DX_VAL_LONG;
                    frame->registers[dst].l = (lb == INT64_MIN && lc == -1) ? 0 : lb % lc;
                    use_int = false; break; }
                case 0xA0: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb & lc; use_int = false; break; }
                case 0xA1: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb | lc; use_int = false; break; }
                case 0xA2: { int64_t lb = frame->registers[b].l; int64_t lc = frame->registers[c].l;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb ^ lc; use_int = false; break; }
                case 0xA3: { int64_t lb = frame->registers[b].l; int32_t shift = frame->registers[c].i;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb << (shift & 0x3F); use_int = false; break; }
                case 0xA4: { int64_t lb = frame->registers[b].l; int32_t shift = frame->registers[c].i;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = lb >> (shift & 0x3F); use_int = false; break; }
                case 0xA5: { int64_t lb = frame->registers[b].l; int32_t shift = frame->registers[c].i;
                    frame->registers[dst].tag = DX_VAL_LONG; frame->registers[dst].l = (int64_t)((uint64_t)lb >> (shift & 0x3F)); use_int = false; break; }
                // Float operations
                case 0xA6: { float fb = frame->registers[b].f; float fc = frame->registers[c].f;
                    frame->registers[dst].tag = DX_VAL_FLOAT; frame->registers[dst].f = fb + fc; use_int = false; break; }
                case 0xA7: { float fb = frame->registers[b].f; float fc = frame->registers[c].f;
                    frame->registers[dst].tag = DX_VAL_FLOAT; frame->registers[dst].f = fb - fc; use_int = false; break; }
                case 0xA8: { float fb = frame->registers[b].f; float fc = frame->registers[c].f;
                    frame->registers[dst].tag = DX_VAL_FLOAT; frame->registers[dst].f = fb * fc; use_int = false; break; }
                case 0xA9: { float fb = frame->registers[b].f; float fc = frame->registers[c].f;
                    frame->registers[dst].tag = DX_VAL_FLOAT; frame->registers[dst].f = fc != 0 ? fb / fc : 0; use_int = false; break; }
                case 0xAA: { float fb = frame->registers[b].f; float fc = frame->registers[c].f;
                    frame->registers[dst].tag = DX_VAL_FLOAT; frame->registers[dst].f = fmodf(fb, fc); use_int = false; break; }
                // Double operations
                case 0xAB: { double db = frame->registers[b].d; double dc = frame->registers[c].d;
                    frame->registers[dst].tag = DX_VAL_DOUBLE; frame->registers[dst].d = db + dc; use_int = false; break; }
                case 0xAC: { double db = frame->registers[b].d; double dc = frame->registers[c].d;
                    frame->registers[dst].tag = DX_VAL_DOUBLE; frame->registers[dst].d = db - dc; use_int = false; break; }
                case 0xAD: { double db = frame->registers[b].d; double dc = frame->registers[c].d;
                    frame->registers[dst].tag = DX_VAL_DOUBLE; frame->registers[dst].d = db * dc; use_int = false; break; }
                case 0xAE: { double db = frame->registers[b].d; double dc = frame->registers[c].d;
                    frame->registers[dst].tag = DX_VAL_DOUBLE; frame->registers[dst].d = dc != 0 ? db / dc : 0; use_int = false; break; }
                case 0xAF: { double db = frame->registers[b].d; double dc = frame->registers[c].d;
                    frame->registers[dst].tag = DX_VAL_DOUBLE; frame->registers[dst].d = fmod(db, dc); use_int = false; break; }
                default: r = 0; break;
            }
            if (use_int) frame->registers[dst] = DX_INT_VALUE(r);
            pc += 2;
            break;
        }

        // Binary operations /2addr (12x): binop/2addr vA, vB
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
            uint8_t a = (inst >> 8) & 0x0F;
            uint8_t b = (inst >> 12) & 0x0F;
            int32_t va = frame->registers[a].i;
            int32_t vb = frame->registers[b].i;
            int32_t r = 0;
            bool use_int_2addr = true;
            switch (opcode) {
                case 0xB0: r = va + vb; break; // add-int/2addr
                case 0xB1: r = va - vb; break; // sub-int/2addr
                case 0xB2: r = va * vb; break; // mul-int/2addr
                case 0xB3: // div-int/2addr
                    if (vb == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        DX_WARN(TAG, "ArithmeticException: divide by zero"); goto done;
                    }
                    r = (va == INT32_MIN && vb == -1) ? INT32_MIN : va / vb; break;
                case 0xB4: // rem-int/2addr
                    if (vb == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        DX_WARN(TAG, "ArithmeticException: divide by zero"); goto done;
                    }
                    r = (va == INT32_MIN && vb == -1) ? 0 : va % vb; break;
                case 0xB5: r = va & vb; break; // and-int/2addr
                case 0xB6: r = va | vb; break; // or-int/2addr
                case 0xB7: r = va ^ vb; break; // xor-int/2addr
                case 0xB8: r = va << (vb & 0x1F); break; // shl-int/2addr
                case 0xB9: r = va >> (vb & 0x1F); break; // shr-int/2addr
                case 0xBA: r = (int32_t)((uint32_t)va >> (vb & 0x1F)); break; // ushr-int/2addr
                // Long operations /2addr
                case 0xBB: { int64_t la = frame->registers[a].l; int64_t lb = frame->registers[b].l;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la + lb; use_int_2addr = false; break; }
                case 0xBC: { int64_t la = frame->registers[a].l; int64_t lb = frame->registers[b].l;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la - lb; use_int_2addr = false; break; }
                case 0xBD: { int64_t la = frame->registers[a].l; int64_t lb = frame->registers[b].l;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la * lb; use_int_2addr = false; break; }
                case 0xBE: { int64_t la = frame->registers[a].l; int64_t lb2 = frame->registers[b].l;
                    if (lb2 == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        goto done;
                    }
                    frame->registers[a].tag = DX_VAL_LONG;
                    frame->registers[a].l = (la == INT64_MIN && lb2 == -1) ? INT64_MIN : la / lb2;
                    use_int_2addr = false; break; }
                case 0xBF: { int64_t la = frame->registers[a].l; int64_t lb2 = frame->registers[b].l;
                    if (lb2 == 0) {
                        DxObject *exc = dx_vm_create_exception(vm, "Ljava/lang/ArithmeticException;", "divide by zero");
                        if (exc && method->code.tries_size > 0) {
                            uint32_t handler = find_catch_handler(vm, frame, code, code_size,
                                                                   method->code.tries_size, pc, exc);
                            if (handler != UINT32_MAX) { pc = handler; goto next_instruction; }
                        }
                        if (exc) { vm->pending_exception = exc; exec_result = DX_ERR_EXCEPTION; goto done; }
                        goto done;
                    }
                    frame->registers[a].tag = DX_VAL_LONG;
                    frame->registers[a].l = (la == INT64_MIN && lb2 == -1) ? 0 : la % lb2;
                    use_int_2addr = false; break; }
                case 0xC0: { int64_t la = frame->registers[a].l; int64_t lb = frame->registers[b].l;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la & lb; use_int_2addr = false; break; }
                case 0xC1: { int64_t la = frame->registers[a].l; int64_t lb = frame->registers[b].l;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la | lb; use_int_2addr = false; break; }
                case 0xC2: { int64_t la = frame->registers[a].l; int64_t lb = frame->registers[b].l;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la ^ lb; use_int_2addr = false; break; }
                case 0xC3: { int64_t la = frame->registers[a].l; int32_t shift = frame->registers[b].i;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la << (shift & 0x3F); use_int_2addr = false; break; }
                case 0xC4: { int64_t la = frame->registers[a].l; int32_t shift = frame->registers[b].i;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = la >> (shift & 0x3F); use_int_2addr = false; break; }
                case 0xC5: { int64_t la = frame->registers[a].l; int32_t shift = frame->registers[b].i;
                    frame->registers[a].tag = DX_VAL_LONG; frame->registers[a].l = (int64_t)((uint64_t)la >> (shift & 0x3F)); use_int_2addr = false; break; }
                // Float operations /2addr
                case 0xC6: { float fa = frame->registers[a].f; float fb2 = frame->registers[b].f;
                    frame->registers[a].tag = DX_VAL_FLOAT; frame->registers[a].f = fa + fb2; use_int_2addr = false; break; }
                case 0xC7: { float fa = frame->registers[a].f; float fb2 = frame->registers[b].f;
                    frame->registers[a].tag = DX_VAL_FLOAT; frame->registers[a].f = fa - fb2; use_int_2addr = false; break; }
                case 0xC8: { float fa = frame->registers[a].f; float fb2 = frame->registers[b].f;
                    frame->registers[a].tag = DX_VAL_FLOAT; frame->registers[a].f = fa * fb2; use_int_2addr = false; break; }
                case 0xC9: { float fa = frame->registers[a].f; float fb2 = frame->registers[b].f;
                    frame->registers[a].tag = DX_VAL_FLOAT; frame->registers[a].f = fb2 != 0 ? fa / fb2 : 0; use_int_2addr = false; break; }
                case 0xCA: { float fa = frame->registers[a].f; float fb2 = frame->registers[b].f;
                    frame->registers[a].tag = DX_VAL_FLOAT; frame->registers[a].f = fmodf(fa, fb2); use_int_2addr = false; break; }
                // Double operations /2addr
                case 0xCB: { double da = frame->registers[a].d; double db2 = frame->registers[b].d;
                    frame->registers[a].tag = DX_VAL_DOUBLE; frame->registers[a].d = da + db2; use_int_2addr = false; break; }
                case 0xCC: { double da = frame->registers[a].d; double db2 = frame->registers[b].d;
                    frame->registers[a].tag = DX_VAL_DOUBLE; frame->registers[a].d = da - db2; use_int_2addr = false; break; }
                case 0xCD: { double da = frame->registers[a].d; double db2 = frame->registers[b].d;
                    frame->registers[a].tag = DX_VAL_DOUBLE; frame->registers[a].d = da * db2; use_int_2addr = false; break; }
                case 0xCE: { double da = frame->registers[a].d; double db2 = frame->registers[b].d;
                    frame->registers[a].tag = DX_VAL_DOUBLE; frame->registers[a].d = db2 != 0 ? da / db2 : 0; use_int_2addr = false; break; }
                case 0xCF: { double da = frame->registers[a].d; double db2 = frame->registers[b].d;
                    frame->registers[a].tag = DX_VAL_DOUBLE; frame->registers[a].d = fmod(da, db2); use_int_2addr = false; break; }
                default: r = 0; break;
            }
            if (use_int_2addr) frame->registers[a] = DX_INT_VALUE(r);
            pc += 1;
            break;
        }

        // binop/lit16 (22s): binop/lit16 vA, vB, #+CCCC
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
            uint8_t dst = (inst >> 8) & 0x0F;
            uint8_t src = (inst >> 12) & 0x0F;
            int16_t lit = (int16_t)code[pc + 1];
            int32_t va = frame->registers[src].i;
            int32_t r = 0;
            switch (opcode) {
                case 0xD0: r = va + lit; break; // add-int/lit16
                case 0xD1: r = (int32_t)lit - va; break; // rsub-int
                case 0xD2: r = va * lit; break; // mul-int/lit16
                case 0xD3: r = lit ? ((va == INT32_MIN && lit == -1) ? INT32_MIN : va / lit) : 0; break;
                case 0xD4: r = lit ? ((va == INT32_MIN && lit == -1) ? 0 : va % lit) : 0; break;
                case 0xD5: r = va & lit; break; // and-int/lit16
                case 0xD6: r = va | lit; break; // or-int/lit16
                case 0xD7: r = va ^ lit; break; // xor-int/lit16
            }
            frame->registers[dst] = DX_INT_VALUE(r);
            pc += 2;
            break;
        }

        // binop/lit8 (22b): binop/lit8 vAA, vBB, #+CC
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        case 0xE0: case 0xE1: case 0xE2: {
            uint8_t dst = (inst >> 8) & 0xFF;
            uint8_t src = code[pc + 1] & 0xFF;
            int8_t lit = (int8_t)((code[pc + 1] >> 8) & 0xFF);
            int32_t va = frame->registers[src].i;
            int32_t r = 0;
            switch (opcode) {
                case 0xD8: r = va + lit; break; // add-int/lit8
                case 0xD9: r = (int32_t)lit - va; break; // rsub-int/lit8
                case 0xDA: r = va * lit; break; // mul-int/lit8
                case 0xDB: r = lit ? ((va == INT32_MIN && lit == -1) ? INT32_MIN : va / lit) : 0; break;
                case 0xDC: r = lit ? ((va == INT32_MIN && lit == -1) ? 0 : va % lit) : 0; break;
                case 0xDD: r = va & lit; break; // and-int/lit8
                case 0xDE: r = va | lit; break; // or-int/lit8
                case 0xDF: r = va ^ lit; break; // xor-int/lit8
                case 0xE0: r = va << (lit & 0x1F); break; // shl-int/lit8
                case 0xE1: r = va >> (lit & 0x1F); break; // shr-int/lit8
                case 0xE2: r = (int32_t)((uint32_t)va >> (lit & 0x1F)); break; // ushr-int/lit8
            }
            frame->registers[dst] = DX_INT_VALUE(r);
            pc += 2;
            break;
        }

        // invoke-polymorphic (45cc format, 4 code units)
        // invoke-polymorphic/range (4rcc format, 4 code units)
        case 0xFA: case 0xFB: {
            DX_WARN(TAG, "%s at pc=%u in %s.%s - not supported, returning null",
                     dx_opcode_name(opcode), pc,
                     method->declaring_class ? method->declaring_class->descriptor : "?",
                     method->name);
            frame->result = DX_NULL_VALUE;
            frame->has_result = true;
            pc += 4;
            break;
        }

        // invoke-custom (35c format, 3 code units)
        // invoke-custom/range (3rc format, 3 code units)
        case 0xFC: case 0xFD: {
            DX_WARN(TAG, "%s at pc=%u in %s.%s - not supported, returning null (most APKs desugar lambdas via R8)",
                     dx_opcode_name(opcode), pc,
                     method->declaring_class ? method->declaring_class->descriptor : "?",
                     method->name);
            frame->result = DX_NULL_VALUE;
            frame->has_result = true;
            pc += 3;
            break;
        }

        // const-method-handle (21c format, 2 code units)
        // const-method-type (21c format, 2 code units)
        case 0xFE: case 0xFF: {
            uint8_t dst = (inst >> 8) & 0xFF;
            DX_WARN(TAG, "%s at pc=%u in %s.%s - not supported, storing null in v%u",
                     dx_opcode_name(opcode), pc,
                     method->declaring_class ? method->declaring_class->descriptor : "?",
                     method->name, dst);
            frame->registers[dst] = DX_NULL_VALUE;
            pc += 2;
            break;
        }

        default: {
            const char *op_name = dx_opcode_name(opcode);
            const char *cls_desc = method->declaring_class ? method->declaring_class->descriptor : "?";
            const char *mth_name = method->name ? method->name : "?";
            DX_WARN(TAG, "Unsupported opcode 0x%02x (%s) at pc=%u in %s.%s - skipping",
                     opcode, op_name, pc, cls_desc, mth_name);
            snprintf(vm->error_msg, sizeof(vm->error_msg),
                     "Unsupported feature: opcode 0x%02x (%s) at pc=%u in %s.%s",
                     opcode, op_name, pc, cls_desc, mth_name);
            // Skip by instruction width instead of failing
            uint32_t width = dx_opcode_width(opcode);
            pc += width;
            break;
        }
        }
    }

done:
    #undef CODE_AT
    #undef CHECK_REG
    #undef INSN_TRACE_SIZE

    // Before leaving the method on an exception path, check if the current PC
    // falls inside a try block with a catch-all (finally) handler that wasn't
    // already tried by the inline exception dispatch.  This covers cases where
    // an exception was created + goto done without going through find_catch_handler
    // (e.g. some runtime errors), or where find_catch_handler matched a typed
    // handler but there's also a finally on an outer try block.
    if (exec_result == DX_ERR_EXCEPTION && vm->pending_exception &&
        method->code.tries_size > 0) {
        uint32_t finally_addr = find_catch_handler(vm, frame, code, code_size,
                                                    method->code.tries_size, pc,
                                                    vm->pending_exception);
        if (finally_addr != UINT32_MAX) {
            DX_DEBUG(TAG, "Exception finally handler at %u in %s.%s (exit path)",
                     finally_addr,
                     method->declaring_class ? method->declaring_class->descriptor : "?",
                     method->name);
            vm->pending_exception = NULL;
            exec_result = DX_OK;
            pc = finally_addr;
            goto next_instruction;
        }
    }

    // Capture diagnostic info on error
    if (exec_result != DX_OK) {
        vm->diag.has_error = true;
        snprintf(vm->diag.method_name, sizeof(vm->diag.method_name), "%s.%s",
                 method->declaring_class ? method->declaring_class->descriptor : "?",
                 method->name ? method->name : "?");
        vm->diag.pc = pc;
        if (pc < code_size) {
            vm->diag.opcode = code[pc] & 0xFF;
        } else {
            vm->diag.opcode = 0;
        }
        snprintf(vm->diag.opcode_name, sizeof(vm->diag.opcode_name), "%s",
                 dx_opcode_name(vm->diag.opcode));

        // Snapshot registers (up to 16)
        uint16_t snap_count = method->code.registers_size;
        if (snap_count > 16) snap_count = 16;
        vm->diag.reg_count = snap_count;
        for (uint32_t r = 0; r < snap_count; r++) {
            vm->diag.registers[r] = frame->registers[r];
        }

        // Build stack trace from frame chain
        int spos = 0;
        vm->diag.stack_trace[0] = '\0';
        DxFrame *sf = frame;
        int depth = 0;
        while (sf && depth < 32 && spos < (int)sizeof(vm->diag.stack_trace) - 80) {
            if (sf->method) {
                spos += snprintf(vm->diag.stack_trace + spos,
                                 sizeof(vm->diag.stack_trace) - spos,
                                 "    at %s.%s (pc=%u)\n",
                                 sf->method->declaring_class ? sf->method->declaring_class->descriptor : "?",
                                 sf->method->name ? sf->method->name : "?",
                                 sf->pc);
            }
            sf = sf->caller;
            depth++;
        }
    }

    vm->stack_depth--;
    vm->current_frame = frame->caller;

    if (frame->has_result && result) {
        *result = frame->result;
    }

    dx_vm_free_frame(vm, frame);
    return exec_result;
}
