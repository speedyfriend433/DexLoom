#ifndef DX_VM_H
#define DX_VM_H

#include "dx_types.h"
#include "dx_dex.h"

// Class status
typedef enum {
    DX_CLASS_NOT_LOADED = 0,
    DX_CLASS_LOADING,
    DX_CLASS_LOADED,
    DX_CLASS_INITIALIZING,
    DX_CLASS_INITIALIZED,
    DX_CLASS_ERROR,
} DxClassStatus;

// Runtime class representation
struct DxClass {
    const char      *descriptor;        // e.g., "Lcom/example/Main;"
    DxClass         *super_class;
    DxClassStatus    status;
    uint32_t         access_flags;

    // Interfaces
    const char     **interfaces;        // interface descriptors this class implements
    uint32_t         interface_count;

    // Fields
    uint32_t         instance_field_count;
    uint32_t         static_field_count;
    struct {
        const char  *name;
        const char  *type;
        uint32_t     flags;
    } *field_defs;
    DxValue         *static_fields;     // array[static_field_count]

    // Methods
    DxMethod        *direct_methods;
    uint32_t         direct_method_count;
    DxMethod        *virtual_methods;
    uint32_t         virtual_method_count;

    // VTable (flattened: super virtuals + own virtuals)
    DxMethod       **vtable;
    uint32_t         vtable_size;

    // Annotations
    struct {
        const char *type;     // annotation type descriptor e.g. "Lretrofit2/http/GET;"
        uint8_t visibility;   // 0=BUILD, 1=RUNTIME, 2=SYSTEM
    } *annotations;
    uint32_t annotation_count;

    // DEX origin
    DxDexFile       *dex_file;          // which DEX file this class came from
    uint32_t         dex_class_def_idx;
    bool             is_framework;      // true for built-in Android stubs
};

// Native method implementation signature
typedef DxResult (*DxNativeMethodFn)(DxVM *vm, DxFrame *frame, DxValue *args, uint32_t arg_count);

// Runtime method representation
struct DxMethod {
    const char        *name;
    const char        *shorty;          // return+param type chars
    DxClass           *declaring_class;
    uint32_t           access_flags;
    uint32_t           dex_method_idx;

    // Bytecode (NULL for native methods)
    DxDexCodeItem      code;
    bool               has_code;

    // Native implementation (for framework stubs)
    DxNativeMethodFn   native_fn;
    bool               is_native;

    // VTable index (-1 if not virtual)
    int32_t            vtable_idx;

    // Annotations
    struct {
        const char *type;     // annotation type descriptor
        uint8_t visibility;   // 0=BUILD, 1=RUNTIME, 2=SYSTEM
    } *annotations;
    uint32_t           annotation_count;
};

// Runtime object
struct DxObject {
    DxClass   *klass;
    DxValue   *fields;          // array[klass->instance_field_count]
    uint32_t   ref_count;
    uint32_t   heap_idx;        // index in VM heap
    bool       gc_mark;         // used by mark-sweep GC

    // For View objects: link to UI node
    DxUINode  *ui_node;

    // Array support
    bool       is_array;
    uint32_t   array_length;
    DxValue   *array_elements;  // array[array_length] for array objects
};

// Interpreter frame (heap-allocated per method call)
struct DxFrame {
    DxMethod  *method;
    DxValue    registers[DX_MAX_REGISTERS];
    uint32_t   pc;              // program counter (in 16-bit code units)
    DxFrame   *caller;
    DxValue    result;          // return value from callee
    bool       has_result;
    DxObject  *exception;       // pending exception for try/catch handling
};

// VM state
#define DX_MAX_DEX_FILES 8

struct DxVM {
    DxContext  *ctx;
    DxDexFile *dex;              // primary DEX (for backwards compat)
    DxDexFile *dex_files[DX_MAX_DEX_FILES];
    uint32_t   dex_count;

    // Class table
    DxClass   *classes[DX_MAX_CLASSES];
    uint32_t   class_count;

    // Class hash table for O(1) lookup
    #define DX_CLASS_HASH_SIZE 4096
    struct {
        const char *descriptor;  // key (points to DxClass->descriptor)
        DxClass    *cls;         // value
    } class_hash[DX_CLASS_HASH_SIZE];

    // Heap
    DxObject  *heap[DX_MAX_HEAP_OBJECTS];
    uint32_t   heap_count;

    // Call stack
    DxFrame   *current_frame;
    uint32_t   stack_depth;

    // Framework classes (pre-registered)
    DxClass   *class_object;        // java/lang/Object
    DxClass   *class_string;        // java/lang/String
    DxClass   *class_activity;      // android/app/Activity
    DxClass   *class_view;          // android/view/View
    DxClass   *class_textview;      // android/widget/TextView
    DxClass   *class_button;        // android/widget/Button
    DxClass   *class_viewgroup;     // android/view/ViewGroup
    DxClass   *class_linearlayout;  // android/widget/LinearLayout
    DxClass   *class_context;       // android/content/Context
    DxClass   *class_bundle;        // android/os/Bundle
    DxClass   *class_resources;     // android/content/res/Resources
    DxClass   *class_onclick;       // android/view/View$OnClickListener
    DxClass   *class_appcompat;     // androidx/.../AppCompatActivity
    DxClass   *class_edittext;      // android/widget/EditText
    DxClass   *class_imageview;     // android/widget/ImageView
    DxClass   *class_toast;         // android/widget/Toast
    DxClass   *class_log;           // android/util/Log
    DxClass   *class_intent;        // android/content/Intent
    DxClass   *class_shared_prefs;  // android/content/SharedPreferences
    DxClass   *class_inflater;      // android/view/LayoutInflater
    DxClass   *class_arraylist;     // java/util/ArrayList
    DxClass   *class_hashmap;       // java/util/HashMap

    // Current activity instance
    DxObject  *activity_instance;

    // Activity back-stack for startActivityForResult / finish
    #define DX_MAX_ACTIVITY_STACK 16
    struct {
        DxObject *activity;     // the Activity object
        DxObject *intent;       // Intent that launched it
        int32_t   request_code; // -1 if plain startActivity
        DxObject *saved_state;  // Bundle from onSaveInstanceState (NULL if none)
    } activity_stack[DX_MAX_ACTIVITY_STACK];
    uint32_t activity_stack_depth;

    // Per-activity result state (set via setResult before finish)
    int32_t   activity_result_code;    // RESULT_CANCELED=0 by default
    DxObject *activity_result_data;    // optional Intent

    // String intern table
    #define DX_MAX_INTERNED_STRINGS 8192
    struct { char *value; DxObject *obj; } interned_strings[DX_MAX_INTERNED_STRINGS];
    uint32_t   interned_count;

    // Execution state
    bool       running;
    DxResult   last_error;
    char       error_msg[256];
    uint64_t   insn_count;      // Instructions executed in current top-level call
    uint64_t   insn_total;      // Lifetime total instructions (for stats)
    uint64_t   insn_limit;      // Max instructions per top-level call (0 = unlimited)

    // Frame pool for interpreter performance
    #define DX_FRAME_POOL_SIZE 64
    DxFrame  *frame_pool[DX_FRAME_POOL_SIZE];
    uint32_t  frame_pool_count;

    // Pending exception for cross-method unwinding
    DxObject  *pending_exception;

    // Diagnostic info captured on error
    struct {
        bool     has_error;
        char     method_name[128];    // "Lcom/example/Foo;.bar"
        uint32_t pc;                  // program counter at error
        uint8_t  opcode;              // opcode at error
        char     opcode_name[32];     // human-readable opcode name
        uint32_t reg_count;           // number of registers to show
        DxValue  registers[16];       // snapshot of first 16 registers
        char     stack_trace[2048];   // formatted call chain
    } diag;

    // SharedPreferences in-memory store (simple key-value)
    #define DX_MAX_PREFS_ENTRIES 256
    struct {
        char    *key;
        DxValue  value;
    } prefs[DX_MAX_PREFS_ENTRIES];
    uint32_t prefs_count;
};

// VM lifecycle
DxVM    *dx_vm_create(DxContext *ctx);
void     dx_vm_destroy(DxVM *vm);
DxResult dx_vm_load_dex(DxVM *vm, DxDexFile *dex);
DxResult dx_vm_register_framework_classes(DxVM *vm);

// Class operations
DxResult dx_vm_load_class(DxVM *vm, const char *descriptor, DxClass **out);
DxResult dx_vm_init_class(DxVM *vm, DxClass *cls);
DxClass *dx_vm_find_class(DxVM *vm, const char *descriptor);

// Garbage collection
void      dx_vm_gc(DxVM *vm);

// Object operations
DxObject *dx_vm_alloc_object(DxVM *vm, DxClass *cls);
DxObject *dx_vm_alloc_array(DxVM *vm, uint32_t length);
void      dx_vm_release_object(DxVM *vm, DxObject *obj);
DxResult  dx_vm_set_field(DxObject *obj, const char *name, DxValue value);
DxResult  dx_vm_get_field(DxObject *obj, const char *name, DxValue *out);

// Exception creation
DxObject *dx_vm_create_exception(DxVM *vm, const char *class_descriptor, const char *message);

// String operations
DxObject *dx_vm_create_string(DxVM *vm, const char *utf8);
DxObject *dx_vm_intern_string(DxVM *vm, const char *utf8);
const char *dx_vm_get_string_value(DxObject *str_obj);

// Method resolution
DxMethod *dx_vm_resolve_method(DxVM *vm, uint32_t dex_method_idx);
DxMethod *dx_vm_find_method(DxClass *cls, const char *name, const char *shorty);
DxMethod *dx_vm_find_interface_method(DxVM *vm, DxClass *cls, const char *name, const char *shorty);

// Frame pool
DxFrame *dx_vm_alloc_frame(DxVM *vm);
void     dx_vm_free_frame(DxVM *vm, DxFrame *frame);

// Execution
DxResult dx_vm_execute_method(DxVM *vm, DxMethod *method, DxValue *args, uint32_t arg_count, DxValue *result);
DxResult dx_vm_run_main_activity(DxVM *vm, const char *activity_class);

// Diagnostics
char *dx_vm_heap_stats(DxVM *vm);
char *dx_vm_get_last_error_detail(DxVM *vm);

// Crash isolation (signal-based recovery)
#include <setjmp.h>
void        dx_crash_install_handlers(DxVM *vm);
void        dx_crash_uninstall_handlers(void);
int         dx_crash_get_signal(void);
sigjmp_buf *dx_crash_get_jmpbuf(void);

#endif // DX_VM_H
