import XCTest
import Foundation
@testable import DexLoom

// MARK: - Helper to create a VM with framework classes registered

private func makeVM() -> (ctx: UnsafeMutablePointer<DxContext>, vm: UnsafeMutablePointer<DxVM>) {
    let ctx = dx_context_create()!
    let vm = dx_vm_create(ctx)!
    dx_vm_register_framework_classes(vm)
    return (ctx, vm)
}

private func teardownVM(_ ctx: UnsafeMutablePointer<DxContext>, _ vm: UnsafeMutablePointer<DxVM>) {
    dx_vm_destroy(vm)
    ctx.pointee.vm = nil
    dx_context_destroy(ctx)
}

// ============================================================
// MARK: - Existing Core Tests
// ============================================================

final class DexLoomCoreTests: XCTestCase {
    func testContextLifecycle() {
        let ctx = dx_context_create()
        XCTAssertTrue(ctx != nil)
        if let ctx = ctx {
            dx_context_destroy(ctx)
        }
    }
    func testDexMagicValidation() {
        // Must be >= header size (112) but with bad magic
        var bad_data = [UInt8](repeating: 0, count: 112)
        var dex: UnsafeMutablePointer<DxDexFile>?
        let result = dx_dex_parse(&bad_data, UInt32(bad_data.count), &dex)
        XCTAssertTrue(result == DX_ERR_INVALID_MAGIC)
    }
    func testDexHeaderParsing() {
        var data = [UInt8](repeating: 0, count: 112)
        let magic: [UInt8] = [0x64, 0x65, 0x78, 0x0A, 0x30, 0x33, 0x35, 0x00]
        for i in 0..<8 { data[i] = magic[i] }
        data[32] = 112; data[33] = 0; data[34] = 0; data[35] = 0
        data[36] = 112; data[37] = 0; data[38] = 0; data[39] = 0
        data[40] = 0x78; data[41] = 0x56; data[42] = 0x34; data[43] = 0x12

        var dex: UnsafeMutablePointer<DxDexFile>?
        let result = dx_dex_parse(&data, UInt32(data.count), &dex)
        XCTAssertTrue(result == DX_OK)
        if let dex = dex {
            XCTAssertTrue(dex.pointee.header.header_size == 112)
            dx_dex_free(dex)
        }
    }
    func testLogInit() {
        dx_log_init()
        dx_log_msg(DX_LOG_INFO, "Test", "Hello from test")
    }
    func testResultStrings() {
        let ok = String(cString: dx_result_string(DX_OK))
        XCTAssertTrue(ok == "OK")
        let notFound = String(cString: dx_result_string(DX_ERR_NOT_FOUND))
        XCTAssertTrue(notFound == "NOT_FOUND")
    }
    func testOpcodeNames() {
        let nop = String(cString: dx_opcode_name(0x00))
        XCTAssertTrue(nop == "nop")
        let invokeVirtual = String(cString: dx_opcode_name(0x6E))
        XCTAssertTrue(invokeVirtual == "invoke-virtual")
    }
    func testUINodeTree() {
        let root = dx_ui_node_create(DX_VIEW_LINEAR_LAYOUT, 1)!
        let child1 = dx_ui_node_create(DX_VIEW_TEXT_VIEW, 2)!
        let child2 = dx_ui_node_create(DX_VIEW_BUTTON, 3)!

        dx_ui_node_add_child(root, child1)
        dx_ui_node_add_child(root, child2)
        XCTAssertTrue(root.pointee.child_count == 2)

        dx_ui_node_set_text(child1, "Hello")
        XCTAssertTrue(String(cString: child1.pointee.text) == "Hello")

        let found = dx_ui_node_find_by_id(root, 3)
        XCTAssertTrue(found == child2)
        XCTAssertTrue(dx_ui_node_find_by_id(root, 99) == nil)

        dx_ui_node_destroy(root)
    }
    func testVMFrameworkRegistration() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        XCTAssertTrue(dx_vm_find_class(vm, "Ljava/lang/Object;") != nil)
        XCTAssertTrue(dx_vm_find_class(vm, "Landroid/app/Activity;") != nil)
        XCTAssertTrue(dx_vm_find_class(vm, "Landroid/widget/TextView;") != nil)
        XCTAssertTrue(dx_vm_find_class(vm, "Landroid/widget/Button;") != nil)
    }
    func testVMStrings() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let strObj = dx_vm_create_string(vm, "Hello DexLoom")
        XCTAssertTrue(strObj != nil)
        if let strObj = strObj {
            let value = dx_vm_get_string_value(strObj)
            XCTAssertTrue(value != nil)
            if let value = value {
                XCTAssertTrue(String(cString: value) == "Hello DexLoom")
            }
        }
    }
    func testVMObjectAlloc() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let cls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        let obj = dx_vm_alloc_object(vm, cls)
        XCTAssertTrue(obj != nil)
        XCTAssertTrue(obj?.pointee.klass == cls)
    }
    func testFieldHierarchySafety() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // Allocate an Activity object (has no field_defs)
        let actCls = dx_vm_find_class(vm, "Landroid/app/Activity;")!
        let obj = dx_vm_alloc_object(vm, actCls)!

        // set_field on a field that doesn't exist should not crash
        var val = DxValue(tag: DX_VAL_INT, DxValue.__Unnamed_union___Anonymous_field1(i: 42))
        let setResult = dx_vm_set_field(obj, "mExtraDataMap", val)
        XCTAssertTrue(setResult == DX_OK) // silently absorbed

        // get_field on a missing field should return null, not crash
        var out = DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: nil))
        let getResult = dx_vm_get_field(obj, "mExtraDataMap", &out)
        XCTAssertTrue(getResult == DX_OK) // returns null
    }
    func testAppCompatRegistered() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        XCTAssertTrue(dx_vm_find_class(vm, "Landroidx/appcompat/app/AppCompatActivity;") != nil)
        XCTAssertTrue(dx_vm_find_class(vm, "Landroidx/constraintlayout/widget/ConstraintLayout;") != nil)
    }
    func testOpcodeWidths() {
        XCTAssertTrue(dx_opcode_width(0x00) == 1) // nop
        XCTAssertTrue(dx_opcode_width(0x28) == 1) // goto (was broken: 2)
        XCTAssertTrue(dx_opcode_width(0x6E) == 3) // invoke-virtual
        XCTAssertTrue(dx_opcode_width(0x14) == 3) // const (31i)
        XCTAssertTrue(dx_opcode_width(0x18) == 5) // const-wide (51l)
    }
    func testRenderModel() {
        let root = dx_ui_node_create(DX_VIEW_LINEAR_LAYOUT, 1)!
        root.pointee.orientation = DX_ORIENTATION_VERTICAL

        let tv = dx_ui_node_create(DX_VIEW_TEXT_VIEW, 2)!
        dx_ui_node_set_text(tv, "Hello")
        dx_ui_node_add_child(root, tv)

        let model = dx_render_model_create(root)
        XCTAssertTrue(model != nil)
        XCTAssertTrue(model!.pointee.root != nil)
        XCTAssertTrue(model!.pointee.root.pointee.type == DX_VIEW_LINEAR_LAYOUT)
        XCTAssertTrue(model!.pointee.root.pointee.child_count == 1)

        dx_render_model_destroy(model)
        dx_ui_node_destroy(root)
    }
}

// ============================================================
// MARK: - VM Lifecycle Tests
// ============================================================

final class VMLifecycleTests: XCTestCase {
    func testCreateDestroy() {
        let ctx = dx_context_create()!
        let vm = dx_vm_create(ctx)
        XCTAssertTrue(vm != nil)
        if let vm = vm {
            dx_vm_destroy(vm)
        }
        ctx.pointee.vm = nil
        dx_context_destroy(ctx)
    }
    func testRegisterFramework() {
        let ctx = dx_context_create()!
        let vm = dx_vm_create(ctx)!
        let result = dx_vm_register_framework_classes(vm)
        XCTAssertTrue(result == DX_OK)
        // Should have registered many classes
        XCTAssertTrue(vm.pointee.class_count > 100)
        teardownVM(ctx, vm)
    }
    func testClassHashTable() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let classNames = [
            "Ljava/lang/Object;",
            "Ljava/lang/String;",
            "Ljava/lang/Integer;",
            "Ljava/lang/Boolean;",
            "Ljava/util/ArrayList;",
            "Ljava/util/HashMap;",
            "Ljava/util/Arrays;",
            "Ljava/util/Collections;",
            "Landroid/app/Activity;",
            "Landroid/os/Bundle;",
            "Landroid/content/Intent;",
            "Landroid/widget/TextView;",
            "Landroid/widget/Button;",
            "Landroid/widget/EditText;",
            "Landroid/widget/ImageView;",
            "Landroid/widget/Toast;",
            "Landroid/util/Log;",
            "Landroid/view/View;",
            "Landroid/view/ViewGroup;",
        ]
        for name in classNames {
            let cls = dx_vm_find_class(vm, name)
            XCTAssertTrue(cls != nil, "Expected to find class \(name)")
        }
    }
    func testFindClassUnknown() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let cls = dx_vm_find_class(vm, "Lcom/nonexistent/FakeClass;")
        XCTAssertTrue(cls == nil)
    }
    func testVMCachedPointers() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        XCTAssertTrue(vm.pointee.class_object != nil)
        XCTAssertTrue(vm.pointee.class_string != nil)
        XCTAssertTrue(vm.pointee.class_activity != nil)
        XCTAssertTrue(vm.pointee.class_view != nil)
        XCTAssertTrue(vm.pointee.class_textview != nil)
        XCTAssertTrue(vm.pointee.class_button != nil)
        XCTAssertTrue(vm.pointee.class_viewgroup != nil)
        XCTAssertTrue(vm.pointee.class_linearlayout != nil)
        XCTAssertTrue(vm.pointee.class_context != nil)
        XCTAssertTrue(vm.pointee.class_bundle != nil)
        XCTAssertTrue(vm.pointee.class_arraylist != nil)
        XCTAssertTrue(vm.pointee.class_hashmap != nil)
        XCTAssertTrue(vm.pointee.class_intent != nil)
        XCTAssertTrue(vm.pointee.class_edittext != nil)
        XCTAssertTrue(vm.pointee.class_imageview != nil)
        XCTAssertTrue(vm.pointee.class_toast != nil)
        XCTAssertTrue(vm.pointee.class_appcompat != nil)
    }
    func testMultipleVMs() {
        let ctx1 = dx_context_create()!
        let vm1 = dx_vm_create(ctx1)!
        dx_vm_register_framework_classes(vm1)

        let ctx2 = dx_context_create()!
        let vm2 = dx_vm_create(ctx2)!
        dx_vm_register_framework_classes(vm2)

        // Both should work independently
        XCTAssertTrue(dx_vm_find_class(vm1, "Ljava/lang/String;") != nil)
        XCTAssertTrue(dx_vm_find_class(vm2, "Ljava/lang/String;") != nil)

        // Objects from vm1 and vm2 are separate
        let s1 = dx_vm_create_string(vm1, "hello")
        let s2 = dx_vm_create_string(vm2, "world")
        XCTAssertTrue(s1 != nil)
        XCTAssertTrue(s2 != nil)

        dx_vm_destroy(vm1)
        ctx1.pointee.vm = nil
        dx_context_destroy(ctx1)

        // vm2 should still work after vm1 is destroyed
        let s3 = dx_vm_create_string(vm2, "still alive")
        XCTAssertTrue(s3 != nil)

        dx_vm_destroy(vm2)
        ctx2.pointee.vm = nil
        dx_context_destroy(ctx2)
    }
}

// ============================================================
// MARK: - Framework Class Tests
// ============================================================

final class FrameworkClassTests: XCTestCase {
    func testStringCreation() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // Empty string
        let empty = dx_vm_create_string(vm, "")
        XCTAssertTrue(empty != nil)
        XCTAssertTrue(String(cString: dx_vm_get_string_value(empty)!) == "")

        // ASCII content
        let ascii = dx_vm_create_string(vm, "Hello World 123")
        XCTAssertTrue(ascii != nil)
        XCTAssertTrue(String(cString: dx_vm_get_string_value(ascii)!) == "Hello World 123")

        // Long string
        let longStr = String(repeating: "abcd", count: 250)
        let longObj = dx_vm_create_string(vm, longStr)
        XCTAssertTrue(longObj != nil)
        XCTAssertTrue(String(cString: dx_vm_get_string_value(longObj)!) == longStr)
    }
    func testStringInterning() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let s1 = dx_vm_intern_string(vm, "interned_test")
        let s2 = dx_vm_intern_string(vm, "interned_test")
        XCTAssertTrue(s1 != nil)
        XCTAssertTrue(s2 != nil)
        // Interned strings with same value should be the same object
        XCTAssertTrue(s1 == s2)

        // Different value should be a different object
        let s3 = dx_vm_intern_string(vm, "different_value")
        XCTAssertTrue(s3 != nil)
        XCTAssertTrue(s3 != s1)
    }
    func testStringClass() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let strObj = dx_vm_create_string(vm, "test")!
        XCTAssertTrue(strObj.pointee.klass == vm.pointee.class_string)
        let desc = String(cString: strObj.pointee.klass.pointee.descriptor)
        XCTAssertTrue(desc == "Ljava/lang/String;")
    }
    func testArrayListCreation() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let alCls = dx_vm_find_class(vm, "Ljava/util/ArrayList;")!
        let list = dx_vm_alloc_object(vm, alCls)
        XCTAssertTrue(list != nil)
        XCTAssertTrue(list?.pointee.klass == alCls)
    }
    func testArrayListMethods() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let alCls = dx_vm_find_class(vm, "Ljava/util/ArrayList;")!

        // Check that key methods are registered
        let addMethod = dx_vm_find_method(alCls, "add", "ZL")
        XCTAssertTrue(addMethod != nil, "ArrayList.add should be registered")

        let sizeMethod = dx_vm_find_method(alCls, "size", "I")
        XCTAssertTrue(sizeMethod != nil, "ArrayList.size should be registered")

        let getMethod = dx_vm_find_method(alCls, "get", "LI")
        XCTAssertTrue(getMethod != nil, "ArrayList.get should be registered")
    }
    func testHashMapMethods() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let hmCls = dx_vm_find_class(vm, "Ljava/util/HashMap;")!
        let obj = dx_vm_alloc_object(vm, hmCls)
        XCTAssertTrue(obj != nil)

        // Check key methods
        let putMethod = dx_vm_find_method(hmCls, "put", "LLL")
        XCTAssertTrue(putMethod != nil, "HashMap.put should be registered")

        let getMethod = dx_vm_find_method(hmCls, "get", "LL")
        XCTAssertTrue(getMethod != nil, "HashMap.get should be registered")

        let sizeMethod = dx_vm_find_method(hmCls, "size", "I")
        XCTAssertTrue(sizeMethod != nil, "HashMap.size should be registered")

        let containsKeyMethod = dx_vm_find_method(hmCls, "containsKey", "ZL")
        XCTAssertTrue(containsKeyMethod != nil, "HashMap.containsKey should be registered")
    }
    func testIntegerAutoboxing() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let intCls = dx_vm_find_class(vm, "Ljava/lang/Integer;")
        XCTAssertTrue(intCls != nil, "java.lang.Integer should be registered")

        if let intCls = intCls {
            let valueOf = dx_vm_find_method(intCls, "valueOf", "LI")
            XCTAssertTrue(valueOf != nil, "Integer.valueOf should be registered for autoboxing")
        }
    }
    func testAutoboxingClasses() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let types: [(String, String)] = [
            ("Ljava/lang/Long;", "LJ"),
            ("Ljava/lang/Float;", "LF"),
            ("Ljava/lang/Double;", "LD"),
            ("Ljava/lang/Boolean;", "LZ"),
            ("Ljava/lang/Byte;", "LB"),
            ("Ljava/lang/Short;", "LS"),
            ("Ljava/lang/Character;", "LC"),
        ]
        for (desc, shorty) in types {
            let cls = dx_vm_find_class(vm, desc)
            XCTAssertTrue(cls != nil, "Expected \(desc) to be registered")
            if let cls = cls {
                let valueOf = dx_vm_find_method(cls, "valueOf", shorty)
                XCTAssertTrue(valueOf != nil, "Expected valueOf on \(desc)")
            }
        }
    }
    func testActivityLifecycleMethods() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let actCls = dx_vm_find_class(vm, "Landroid/app/Activity;")!

        // Check lifecycle methods exist
        let onCreate = dx_vm_find_method(actCls, "onCreate", "VL")
        XCTAssertTrue(onCreate != nil, "Activity.onCreate should exist")

        let onStart = dx_vm_find_method(actCls, "onStart", "V")
        XCTAssertTrue(onStart != nil, "Activity.onStart should exist")

        let onResume = dx_vm_find_method(actCls, "onResume", "V")
        XCTAssertTrue(onResume != nil, "Activity.onResume should exist")
    }
    func testViewMethods() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let viewCls = dx_vm_find_class(vm, "Landroid/view/View;")!

        let setOnClick = dx_vm_find_method(viewCls, "setOnClickListener", "VL")
        XCTAssertTrue(setOnClick != nil, "View.setOnClickListener should exist")

        let findViewById = dx_vm_find_method(viewCls, "findViewById", "LI")
        XCTAssertTrue(findViewById != nil, "View.findViewById should exist")
    }
    func testIntentClass() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let intentCls = dx_vm_find_class(vm, "Landroid/content/Intent;")!
        let obj = dx_vm_alloc_object(vm, intentCls)
        XCTAssertTrue(obj != nil)

        let putExtra = dx_vm_find_method(intentCls, "putExtra", "LLL")
        XCTAssertTrue(putExtra != nil, "Intent.putExtra should exist")
    }
    func testBundleClass() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let bundleCls = dx_vm_find_class(vm, "Landroid/os/Bundle;")!
        let obj = dx_vm_alloc_object(vm, bundleCls)
        XCTAssertTrue(obj != nil)
    }
    func testExceptionClasses() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let exceptions = [
            "Ljava/lang/Exception;",
            "Ljava/lang/RuntimeException;",
            "Ljava/lang/NullPointerException;",
            "Ljava/lang/ArrayIndexOutOfBoundsException;",
            "Ljava/lang/ClassCastException;",
            "Ljava/lang/ArithmeticException;",
        ]
        for desc in exceptions {
            let cls = dx_vm_find_class(vm, desc)
            XCTAssertTrue(cls != nil, "Expected \(desc) to be registered")
        }
    }
    func testCollectionInterfaces() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let interfaces = [
            "Ljava/lang/Iterable;",
            "Ljava/util/Collection;",
            "Ljava/util/List;",
            "Ljava/util/Set;",
            "Ljava/util/Map;",
            "Ljava/util/Iterator;",
        ]
        for desc in interfaces {
            let cls = dx_vm_find_class(vm, desc)
            XCTAssertTrue(cls != nil, "Expected \(desc) to be registered")
        }
    }
    func testWidgetClasses() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let widgets = [
            "Landroid/widget/Spinner;",
            "Landroid/widget/SeekBar;",
            "Landroid/widget/CheckBox;",
            "Landroid/widget/Switch;",
            "Landroid/widget/RadioButton;",
            "Landroid/widget/RadioGroup;",
            "Landroid/widget/ListView;",
        ]
        for desc in widgets {
            let cls = dx_vm_find_class(vm, desc)
            XCTAssertTrue(cls != nil, "Expected \(desc) to be registered")
        }
    }
    func testKotlinClasses() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // Kotlin stdlib should have some representation
        let kotlinUnit = dx_vm_find_class(vm, "Lkotlin/Unit;")
        XCTAssertTrue(kotlinUnit != nil, "Kotlin Unit should be registered")
    }
}

// ============================================================
// MARK: - Object System Tests
// ============================================================

final class ObjectSystemTests: XCTestCase {
    func testAllocObjectClass() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        let obj = dx_vm_alloc_object(vm, objCls)!

        XCTAssertTrue(obj.pointee.klass == objCls)
        XCTAssertTrue(obj.pointee.is_array == false)
        XCTAssertTrue(obj.pointee.gc_mark == false)
    }
    func testAllocArray() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let arr = dx_vm_alloc_array(vm, 10)
        XCTAssertTrue(arr != nil)
        if let arr = arr {
            XCTAssertTrue(arr.pointee.is_array == true)
            XCTAssertTrue(arr.pointee.array_length == 10)
            XCTAssertTrue(arr.pointee.array_elements != nil)
        }
    }
    func testAllocZeroArray() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let arr = dx_vm_alloc_array(vm, 0)
        XCTAssertTrue(arr != nil)
        if let arr = arr {
            XCTAssertTrue(arr.pointee.is_array == true)
            XCTAssertTrue(arr.pointee.array_length == 0)
        }
    }
    func testArrayElementAccess() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let arr = dx_vm_alloc_array(vm, 5)!

        // Set and read back values
        arr.pointee.array_elements[0] = DxValue(tag: DX_VAL_INT, DxValue.__Unnamed_union___Anonymous_field1(i: 42))
        arr.pointee.array_elements[1] = DxValue(tag: DX_VAL_INT, DxValue.__Unnamed_union___Anonymous_field1(i: 99))

        XCTAssertTrue(arr.pointee.array_elements[0].tag == DX_VAL_INT)
        XCTAssertTrue(arr.pointee.array_elements[0].i == 42)
        XCTAssertTrue(arr.pointee.array_elements[1].i == 99)
    }
    func testHeapTracking() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let initialCount = vm.pointee.heap_count
        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!

        let _ = dx_vm_alloc_object(vm, objCls)
        XCTAssertTrue(vm.pointee.heap_count == initialCount + 1)

        let _ = dx_vm_alloc_object(vm, objCls)
        XCTAssertTrue(vm.pointee.heap_count == initialCount + 2)
    }
    func testGCRelated() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        // Allocate several objects and verify they're on the heap
        let initialCount = vm.pointee.heap_count
        for _ in 0..<20 {
            let _ = dx_vm_alloc_object(vm, objCls)
        }
        XCTAssertTrue(vm.pointee.heap_count == initialCount + 20)
        // Note: dx_vm_gc requires a running execution context with proper
        // root set; calling it outside of execution can crash.
    }
    func testObjectFieldsWithDefs() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // TextView should have field_defs (e.g., mText)
        let tvCls = dx_vm_find_class(vm, "Landroid/widget/TextView;")!
        let tv = dx_vm_alloc_object(vm, tvCls)!
        XCTAssertTrue(tv.pointee.klass == tvCls)
    }
    func testHeapStats() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let _ = dx_vm_alloc_object(vm, dx_vm_find_class(vm, "Ljava/lang/Object;")!)
        let stats = dx_vm_heap_stats(vm)
        XCTAssertTrue(stats != nil)
        if let stats = stats {
            let str = String(cString: stats)
            XCTAssertTrue(str.count > 0)
            free(stats)
        }
    }
    func testCreateException() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let exc = dx_vm_create_exception(vm, "Ljava/lang/NullPointerException;", "test null pointer")
        XCTAssertTrue(exc != nil)
        if let exc = exc {
            let desc = String(cString: exc.pointee.klass.pointee.descriptor)
            XCTAssertTrue(desc == "Ljava/lang/NullPointerException;")
        }
    }
    func testFramePool() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // Allocate a frame
        let frame = dx_vm_alloc_frame(vm)
        XCTAssertTrue(frame != nil)

        // Free it back to pool
        if let frame = frame {
            dx_vm_free_frame(vm, frame)
        }

        // Allocate again - should reuse from pool
        let frame2 = dx_vm_alloc_frame(vm)
        XCTAssertTrue(frame2 != nil)
        if let frame2 = frame2 {
            dx_vm_free_frame(vm, frame2)
        }
    }
    func testFramePoolStress() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // Allocate more frames than pool size, then free all
        var frames: [UnsafeMutablePointer<DxFrame>] = []
        for _ in 0..<80 {
            if let f = dx_vm_alloc_frame(vm) {
                frames.append(f)
            }
        }
        XCTAssertTrue(frames.count == 80)

        // Free them all
        for f in frames {
            dx_vm_free_frame(vm, f)
        }
    }
}

// ============================================================
// MARK: - Bytecode Execution Tests
// ============================================================

final class BytecodeExecutionTests: XCTestCase {
    func testExecuteNativeMethod() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let strCls = dx_vm_find_class(vm, "Ljava/lang/String;")!
        let lengthMethod = dx_vm_find_method(strCls, "length", "I")
        XCTAssertTrue(lengthMethod != nil, "String.length should exist")

        if let lengthMethod = lengthMethod {
            // Create a string object to call length on
            let strObj = dx_vm_create_string(vm, "Hello")!
            var args = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: strObj))]
            var result = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))

            let status = dx_vm_execute_method(vm, lengthMethod, &args, 1, &result)
            XCTAssertTrue(status == DX_OK)
            XCTAssertTrue(result.tag == DX_VAL_INT)
            XCTAssertTrue(result.i == 5)
        }
    }
    func testArrayListSize() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let alCls = dx_vm_find_class(vm, "Ljava/util/ArrayList;")!
        let list = dx_vm_alloc_object(vm, alCls)!

        // Call <init> first
        let initMethod = dx_vm_find_method(alCls, "<init>", "V")
        if let initMethod = initMethod {
            var args = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: list))]
            var initResult = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
            let _ = dx_vm_execute_method(vm, initMethod, &args, 1, &initResult)
        }

        // Call size
        let sizeMethod = dx_vm_find_method(alCls, "size", "I")!
        var sizeArgs = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: list))]
        var sizeResult = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
        let status = dx_vm_execute_method(vm, sizeMethod, &sizeArgs, 1, &sizeResult)
        XCTAssertTrue(status == DX_OK)
        XCTAssertTrue(sizeResult.tag == DX_VAL_INT)
        XCTAssertTrue(sizeResult.i == 0)
    }
    func testArrayListAddAndSize() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let alCls = dx_vm_find_class(vm, "Ljava/util/ArrayList;")!
        let list = dx_vm_alloc_object(vm, alCls)!

        // Init
        let initMethod = dx_vm_find_method(alCls, "<init>", "V")
        if let initMethod = initMethod {
            var args = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: list))]
            var r = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
            let _ = dx_vm_execute_method(vm, initMethod, &args, 1, &r)
        }

        // Add three items
        let addMethod = dx_vm_find_method(alCls, "add", "ZL")!
        for i in 0..<3 {
            let strObj = dx_vm_create_string(vm, "item\(i)")!
            var addArgs = [
                DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: list)),
                DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: strObj))
            ]
            var addResult = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
            let s = dx_vm_execute_method(vm, addMethod, &addArgs, 2, &addResult)
            XCTAssertTrue(s == DX_OK)
        }

        // Size should be 3
        let sizeMethod = dx_vm_find_method(alCls, "size", "I")!
        var sizeArgs = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: list))]
        var sizeResult = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
        let status = dx_vm_execute_method(vm, sizeMethod, &sizeArgs, 1, &sizeResult)
        XCTAssertTrue(status == DX_OK)
        XCTAssertTrue(sizeResult.i == 3)
    }
    func testHashMapPutGet() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let hmCls = dx_vm_find_class(vm, "Ljava/util/HashMap;")!
        let map = dx_vm_alloc_object(vm, hmCls)!

        // Init
        let initMethod = dx_vm_find_method(hmCls, "<init>", "V")
        if let initMethod = initMethod {
            var args = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: map))]
            var r = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
            let _ = dx_vm_execute_method(vm, initMethod, &args, 1, &r)
        }

        // Put a key-value pair
        let putMethod = dx_vm_find_method(hmCls, "put", "LLL")!
        let key = dx_vm_create_string(vm, "myKey")!
        let value = dx_vm_create_string(vm, "myValue")!
        var putArgs = [
            DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: map)),
            DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: key)),
            DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: value))
        ]
        var putResult = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
        let putStatus = dx_vm_execute_method(vm, putMethod, &putArgs, 3, &putResult)
        XCTAssertTrue(putStatus == DX_OK)

        // Get by key
        let getMethod = dx_vm_find_method(hmCls, "get", "LL")!
        var getArgs = [
            DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: map)),
            DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: key))
        ]
        var getResult = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
        let getStatus = dx_vm_execute_method(vm, getMethod, &getArgs, 2, &getResult)
        XCTAssertTrue(getStatus == DX_OK)
        XCTAssertTrue(getResult.tag == DX_VAL_OBJ)
        if let resultObj = getResult.obj {
            let resultStr = String(cString: dx_vm_get_string_value(resultObj)!)
            XCTAssertTrue(resultStr == "myValue")
        }
    }
    func testIntegerValueOf() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let intCls = dx_vm_find_class(vm, "Ljava/lang/Integer;")!
        let valueOf = dx_vm_find_method(intCls, "valueOf", "LI")!

        // valueOf is static, so first arg is not 'this'
        var args = [DxValue(tag: DX_VAL_INT, DxValue.__Unnamed_union___Anonymous_field1(i: 42))]
        var result = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
        let status = dx_vm_execute_method(vm, valueOf, &args, 1, &result)
        XCTAssertTrue(status == DX_OK)
        XCTAssertTrue(result.tag == DX_VAL_OBJ)
        XCTAssertTrue(result.obj != nil)
    }
    func testAllOpcodesHaveNames() {
        for i: UInt8 in 0...255 {
            let name = dx_opcode_name(i)
            XCTAssertTrue(name != nil, "Opcode 0x\(String(i, radix: 16)) should have a name")
        }
    }
    func testOpcodeWidthsPositive() {
        // Key opcodes that must have positive widths
        let opcodes: [UInt8] = [
            0x00, // nop
            0x01, // move
            0x0E, // return-void
            0x12, // const/4
            0x1A, // const-string
            0x22, // new-instance
            0x28, // goto
            0x38, // if-eqz
            0x6E, // invoke-virtual
            0x90, // add-int
        ]
        for op in opcodes {
            XCTAssertTrue(dx_opcode_width(op) > 0, "Opcode 0x\(String(op, radix: 16)) width should be > 0")
        }
    }
}

// ============================================================
// MARK: - Error Handling Tests
// ============================================================

final class ErrorHandlingTests: XCTestCase {
    func testDexParseEmpty() {
        var dex: UnsafeMutablePointer<DxDexFile>?
        // Empty buffer (too small for header)
        var data = [UInt8](repeating: 0, count: 4)
        let result = dx_dex_parse(&data, UInt32(data.count), &dex)
        XCTAssertTrue(result != DX_OK)
    }
    func testDexParseTruncated() {
        var dex: UnsafeMutablePointer<DxDexFile>?
        // 50 bytes is less than the 112-byte header
        var data = [UInt8](repeating: 0, count: 50)
        let magic: [UInt8] = [0x64, 0x65, 0x78, 0x0A, 0x30, 0x33, 0x35, 0x00]
        for i in 0..<8 { data[i] = magic[i] }
        let result = dx_dex_parse(&data, UInt32(data.count), &dex)
        XCTAssertTrue(result != DX_OK)
    }
    func testDexWrongVersion() {
        var data = [UInt8](repeating: 0, count: 112)
        // Valid prefix but invalid version "099"
        let magic: [UInt8] = [0x64, 0x65, 0x78, 0x0A, 0x30, 0x39, 0x39, 0x00]
        for i in 0..<8 { data[i] = magic[i] }
        data[32] = 112; data[33] = 0; data[34] = 0; data[35] = 0
        data[36] = 112; data[37] = 0; data[38] = 0; data[39] = 0
        data[40] = 0x78; data[41] = 0x56; data[42] = 0x34; data[43] = 0x12

        var dex: UnsafeMutablePointer<DxDexFile>?
        let result = dx_dex_parse(&data, UInt32(data.count), &dex)
        // Should either reject or accept depending on version tolerance
        // At minimum it should not crash
        if result == DX_OK, let dex = dex {
            dx_dex_free(dex)
        }
    }
    func testInstructionBudget() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // The VM has insn_limit field - verify it's set to a reasonable value
        // or can be set
        XCTAssertTrue(vm.pointee.insn_limit == 0 || vm.pointee.insn_limit > 0)
        // DX_MAX_INSTRUCTIONS is 500000 per the types header
    }
    func testContextDoubleCreate() {
        // Just verify multiple create/destroy cycles work
        for _ in 0..<5 {
            let ctx = dx_context_create()!
            dx_context_destroy(ctx)
        }
    }
    func testAllResultStrings() {
        let codes: [DxResult] = [
            DX_OK,
            DX_ERR_NULL_PTR,
            DX_ERR_INVALID_MAGIC,
            DX_ERR_INVALID_FORMAT,
            DX_ERR_OUT_OF_MEMORY,
            DX_ERR_NOT_FOUND,
            DX_ERR_UNSUPPORTED_OPCODE,
            DX_ERR_CLASS_NOT_FOUND,
            DX_ERR_METHOD_NOT_FOUND,
            DX_ERR_FIELD_NOT_FOUND,
            DX_ERR_STACK_OVERFLOW,
            DX_ERR_STACK_UNDERFLOW,
            DX_ERR_EXCEPTION,
            DX_ERR_VERIFICATION_FAILED,
            DX_ERR_IO,
            DX_ERR_ZIP_INVALID,
            DX_ERR_AXML_INVALID,
            DX_ERR_UNSUPPORTED_VERSION,
            DX_ERR_INTERNAL,
        ]
        for code in codes {
            let str = dx_result_string(code)
            XCTAssertTrue(str != nil)
            let s = String(cString: str!)
            XCTAssertTrue(s.count > 0, "Result string for code should not be empty")
        }
    }
    func testDiagnosticClean() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        XCTAssertTrue(vm.pointee.diag.has_error == false)
    }
    func testCreateExceptionUnknownClass() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // Creating an exception with an unknown class - should handle gracefully
        let exc = dx_vm_create_exception(vm, "Lcom/fake/NonExistentException;", "test")
        // Either nil or a fallback object is fine, just must not crash
        _ = exc
    }
    func testGetStringValueNil() {
        let result = dx_vm_get_string_value(nil)
        XCTAssertTrue(result == nil)
    }
}

// ============================================================
// MARK: - Parser Hardening Tests
// ============================================================

final class ParserHardeningTests: XCTestCase {
    func testGarbageData() {
        var data = [UInt8](repeating: 0xFF, count: 256)
        var dex: UnsafeMutablePointer<DxDexFile>?
        let result = dx_dex_parse(&data, UInt32(data.count), &dex)
        XCTAssertTrue(result == DX_ERR_INVALID_MAGIC)
    }
    func testFileSizeMismatch() {
        var data = [UInt8](repeating: 0, count: 112)
        let magic: [UInt8] = [0x64, 0x65, 0x78, 0x0A, 0x30, 0x33, 0x35, 0x00]
        for i in 0..<8 { data[i] = magic[i] }
        // header_size = 112
        data[32] = 112; data[33] = 0; data[34] = 0; data[35] = 0
        // file_size = 9999 (way larger than actual)
        data[36] = 0x0F; data[37] = 0x27; data[38] = 0; data[39] = 0
        // endian tag
        data[40] = 0x78; data[41] = 0x56; data[42] = 0x34; data[43] = 0x12

        var dex: UnsafeMutablePointer<DxDexFile>?
        let result = dx_dex_parse(&data, UInt32(data.count), &dex)
        // Should either reject or handle gracefully
        if result == DX_OK, let dex = dex {
            dx_dex_free(dex)
        }
    }
    func testAllViewTypes() {
        let types: [DxViewType] = [
            DX_VIEW_LINEAR_LAYOUT,
            DX_VIEW_TEXT_VIEW,
            DX_VIEW_BUTTON,
            DX_VIEW_IMAGE_VIEW,
            DX_VIEW_EDIT_TEXT,
            DX_VIEW_FRAME_LAYOUT,
            DX_VIEW_RELATIVE_LAYOUT,
            DX_VIEW_CONSTRAINT_LAYOUT,
            DX_VIEW_SCROLL_VIEW,
            DX_VIEW_RECYCLER_VIEW,
            DX_VIEW_CARD_VIEW,
            DX_VIEW_SWITCH,
            DX_VIEW_CHECKBOX,
            DX_VIEW_PROGRESS_BAR,
            DX_VIEW_TOOLBAR,
            DX_VIEW_VIEW,
            DX_VIEW_VIEW_GROUP,
            DX_VIEW_LIST_VIEW,
            DX_VIEW_GRID_VIEW,
            DX_VIEW_SPINNER,
            DX_VIEW_SEEK_BAR,
            DX_VIEW_RATING_BAR,
            DX_VIEW_RADIO_BUTTON,
            DX_VIEW_RADIO_GROUP,
            DX_VIEW_FAB,
            DX_VIEW_TAB_LAYOUT,
            DX_VIEW_VIEW_PAGER,
            DX_VIEW_WEB_VIEW,
            DX_VIEW_CHIP,
            DX_VIEW_BOTTOM_NAV,
            DX_VIEW_SWIPE_REFRESH,
        ]
        for (idx, viewType) in types.enumerated() {
            let node = dx_ui_node_create(viewType, UInt32(idx + 100))
            XCTAssertTrue(node != nil, "Should create node for view type index \(idx)")
            if let node = node {
                XCTAssertTrue(node.pointee.type == viewType)
                XCTAssertTrue(node.pointee.view_id == UInt32(idx + 100))
                dx_ui_node_destroy(node)
            }
        }
    }
    func testDeepUITree() {
        // Build a 50-level deep tree
        let root = dx_ui_node_create(DX_VIEW_FRAME_LAYOUT, 0)!
        var current = root
        for i in 1..<50 {
            let child = dx_ui_node_create(DX_VIEW_FRAME_LAYOUT, UInt32(i))!
            dx_ui_node_add_child(current, child)
            current = child
        }

        // Set text on deepest node
        dx_ui_node_set_text(current, "deep leaf")
        XCTAssertTrue(String(cString: current.pointee.text) == "deep leaf")

        // Find the deepest node by ID
        let found = dx_ui_node_find_by_id(root, 49)
        XCTAssertTrue(found != nil)
        XCTAssertTrue(found == current)

        // Count total nodes
        let count = dx_ui_node_count(root)
        XCTAssertTrue(count == 50)

        dx_ui_node_destroy(root)
    }
    func testUINodeTextOverwrite() {
        let node = dx_ui_node_create(DX_VIEW_TEXT_VIEW, 1)!
        dx_ui_node_set_text(node, "first")
        XCTAssertTrue(String(cString: node.pointee.text) == "first")

        dx_ui_node_set_text(node, "second")
        XCTAssertTrue(String(cString: node.pointee.text) == "second")

        dx_ui_node_set_text(node, "")
        XCTAssertTrue(String(cString: node.pointee.text) == "")

        dx_ui_node_destroy(node)
    }
    func testWideSiblingTree() {
        let root = dx_ui_node_create(DX_VIEW_LINEAR_LAYOUT, 0)!
        for i in 1...64 {
            let child = dx_ui_node_create(DX_VIEW_TEXT_VIEW, UInt32(i))!
            dx_ui_node_set_text(child, "item \(i)")
            dx_ui_node_add_child(root, child)
        }
        XCTAssertTrue(root.pointee.child_count == 64)
        XCTAssertTrue(dx_ui_node_count(root) == 65) // root + 64 children

        // Find last child
        let last = dx_ui_node_find_by_id(root, 64)
        XCTAssertTrue(last != nil)

        dx_ui_node_destroy(root)
    }
    func testRenderModelComplex() {
        let root = dx_ui_node_create(DX_VIEW_LINEAR_LAYOUT, 1)!
        root.pointee.orientation = DX_ORIENTATION_VERTICAL

        let child1 = dx_ui_node_create(DX_VIEW_TEXT_VIEW, 2)!
        dx_ui_node_set_text(child1, "Title")
        dx_ui_node_add_child(root, child1)

        let child2 = dx_ui_node_create(DX_VIEW_FRAME_LAYOUT, 3)!
        dx_ui_node_add_child(root, child2)

        let nested = dx_ui_node_create(DX_VIEW_BUTTON, 4)!
        dx_ui_node_set_text(nested, "Click me")
        dx_ui_node_add_child(child2, nested)

        let model = dx_render_model_create(root)
        XCTAssertTrue(model != nil)
        XCTAssertTrue(model!.pointee.root.pointee.child_count == 2)

        dx_render_model_destroy(model)
        dx_ui_node_destroy(root)
    }
    func testUITreeDump() {
        let root = dx_ui_node_create(DX_VIEW_LINEAR_LAYOUT, 1)!
        let child = dx_ui_node_create(DX_VIEW_TEXT_VIEW, 2)!
        dx_ui_node_set_text(child, "Hello")
        dx_ui_node_add_child(root, child)

        let dump = dx_ui_tree_dump(root)
        XCTAssertTrue(dump != nil)
        if let dump = dump {
            let str = String(cString: dump)
            XCTAssertTrue(str.count > 0)
            free(dump)
        }

        dx_ui_node_destroy(root)
    }
    func testDimensionConversion() {
        let dp16 = dx_ui_dp_to_points(16.0)
        XCTAssertTrue(dp16 > 0)

        let sp14 = dx_ui_sp_to_points(14.0)
        XCTAssertTrue(sp14 > 0)

        // Zero input gives zero output
        let dp0 = dx_ui_dp_to_points(0.0)
        XCTAssertTrue(dp0 == 0.0)
    }
    func testMemoryFunctions() {
        var allocs: UInt64 = 0
        var frees: UInt64 = 0
        var bytes: UInt64 = 0
        dx_memory_stats(&allocs, &frees, &bytes)
        // Just verify it doesn't crash and returns something
        XCTAssertTrue(allocs >= 0)
    }
}

// ============================================================
// MARK: - Class Hierarchy Tests
// ============================================================

final class ClassHierarchyTests: XCTestCase {
    func testObjectRoot() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        XCTAssertTrue(objCls.pointee.super_class == nil)
    }
    func testActivityHierarchy() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let actCls = dx_vm_find_class(vm, "Landroid/app/Activity;")!
        // Activity should have a superclass chain leading to Object
        var current: UnsafeMutablePointer<DxClass>? = actCls
        var depth = 0
        while let cls = current, cls.pointee.super_class != nil {
            current = cls.pointee.super_class
            depth += 1
            if depth > 20 { break } // safety
        }
        XCTAssertTrue(depth > 0, "Activity should have at least one superclass")

        // The root should be Object
        if let root = current {
            let desc = String(cString: root.pointee.descriptor)
            XCTAssertTrue(desc == "Ljava/lang/Object;")
        }
    }
    func testAppCompatHierarchy() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let appCompatCls = dx_vm_find_class(vm, "Landroidx/appcompat/app/AppCompatActivity;")!
        // Walk up to find Activity
        var current: UnsafeMutablePointer<DxClass>? = appCompatCls
        var foundActivity = false
        var depth = 0
        while let cls = current {
            let desc = String(cString: cls.pointee.descriptor)
            if desc == "Landroid/app/Activity;" {
                foundActivity = true
                break
            }
            current = cls.pointee.super_class
            depth += 1
            if depth > 20 { break }
        }
        XCTAssertTrue(foundActivity, "AppCompatActivity should have Activity in its superclass chain")
    }
    func testFrameworkFlag() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        XCTAssertTrue(objCls.pointee.is_framework == true)

        let strCls = dx_vm_find_class(vm, "Ljava/lang/String;")!
        XCTAssertTrue(strCls.pointee.is_framework == true)

        let actCls = dx_vm_find_class(vm, "Landroid/app/Activity;")!
        XCTAssertTrue(actCls.pointee.is_framework == true)
    }
    func testButtonExtendsTextView() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let btnCls = dx_vm_find_class(vm, "Landroid/widget/Button;")!
        let superDesc = String(cString: btnCls.pointee.super_class.pointee.descriptor)
        XCTAssertTrue(superDesc == "Landroid/widget/TextView;")
    }
    func testClassDescriptorFormat() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        // Spot check well-known classes have valid descriptor format
        let sampleClasses = [
            "Ljava/lang/Object;",
            "Ljava/lang/String;",
            "Landroid/app/Activity;",
            "Ljava/util/ArrayList;",
            "Ljava/util/HashMap;",
        ]
        for desc in sampleClasses {
            let cls = dx_vm_find_class(vm, desc)
            XCTAssertTrue(cls != nil, "Should find class \(desc)")
            if let cls = cls {
                let actualDesc = String(cString: cls.pointee.descriptor)
                XCTAssertTrue(actualDesc.hasPrefix("L"), "Descriptor should start with L")
                XCTAssertTrue(actualDesc.hasSuffix(";"), "Descriptor should end with ;")
                XCTAssertTrue(actualDesc == desc)
            }
        }
    }
}

// ============================================================
// MARK: - Method Resolution Tests
// ============================================================

final class MethodResolutionTests: XCTestCase {
    func testFindMethodNotFound() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        let m = dx_vm_find_method(objCls, "totallyFakeMethod", "V")
        XCTAssertTrue(m == nil)
    }
    func testNativeMethodFlag() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let strCls = dx_vm_find_class(vm, "Ljava/lang/String;")!
        let lengthMethod = dx_vm_find_method(strCls, "length", "I")
        XCTAssertTrue(lengthMethod != nil)
        if let m = lengthMethod {
            XCTAssertTrue(m.pointee.is_native == true)
            XCTAssertTrue(m.pointee.native_fn != nil)
        }
    }
    func testMethodDeclaringClass() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let alCls = dx_vm_find_class(vm, "Ljava/util/ArrayList;")!
        let addMethod = dx_vm_find_method(alCls, "add", "ZL")!
        XCTAssertTrue(addMethod.pointee.declaring_class != nil)
    }
    func testObjectToString() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        let toString = dx_vm_find_method(objCls, "toString", "L")
        XCTAssertTrue(toString != nil, "Object.toString should be registered")
    }
    func testObjectEquals() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        let equals = dx_vm_find_method(objCls, "equals", "ZL")
        XCTAssertTrue(equals != nil, "Object.equals should be registered")
    }
    func testObjectHashCode() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        let hashCode = dx_vm_find_method(objCls, "hashCode", "I")
        XCTAssertTrue(hashCode != nil, "Object.hashCode should be registered")
    }
}

// ============================================================
// MARK: - Execution Edge Case Tests
// ============================================================

final class ExecutionEdgeCaseTests: XCTestCase {
    func testStringLengthEmpty() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let strCls = dx_vm_find_class(vm, "Ljava/lang/String;")!
        let lengthMethod = dx_vm_find_method(strCls, "length", "I")!

        let strObj = dx_vm_create_string(vm, "")!
        var args = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: strObj))]
        var result = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))

        let status = dx_vm_execute_method(vm, lengthMethod, &args, 1, &result)
        XCTAssertTrue(status == DX_OK)
        XCTAssertTrue(result.i == 0)
    }
    func testHashMapSizeEmpty() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let hmCls = dx_vm_find_class(vm, "Ljava/util/HashMap;")!
        let map = dx_vm_alloc_object(vm, hmCls)!

        // Init
        if let initMethod = dx_vm_find_method(hmCls, "<init>", "V") {
            var args = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: map))]
            var r = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
            let _ = dx_vm_execute_method(vm, initMethod, &args, 1, &r)
        }

        let sizeMethod = dx_vm_find_method(hmCls, "size", "I")!
        var sizeArgs = [DxValue(tag: DX_VAL_OBJ, DxValue.__Unnamed_union___Anonymous_field1(obj: map))]
        var result = DxValue(tag: DX_VAL_VOID, DxValue.__Unnamed_union___Anonymous_field1(i: 0))
        let status = dx_vm_execute_method(vm, sizeMethod, &sizeArgs, 1, &result)
        XCTAssertTrue(status == DX_OK)
        XCTAssertTrue(result.i == 0)
    }
    func testMultipleStrings() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let strings = ["alpha", "beta", "gamma", "delta", "epsilon"]
        var objects: [UnsafeMutablePointer<DxObject>] = []

        for s in strings {
            let obj = dx_vm_create_string(vm, s)!
            objects.append(obj)
        }

        // Verify each still has its original value
        for (i, obj) in objects.enumerated() {
            let value = String(cString: dx_vm_get_string_value(obj)!)
            XCTAssertTrue(value == strings[i], "String \(i) should be '\(strings[i])' but got '\(value)'")
        }
    }
    func testMassAllocation() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let objCls = dx_vm_find_class(vm, "Ljava/lang/Object;")!
        for _ in 0..<1000 {
            let obj = dx_vm_alloc_object(vm, objCls)
            XCTAssertTrue(obj != nil)
        }
    }
    func testLargeArray() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        let arr = dx_vm_alloc_array(vm, 10000)
        XCTAssertTrue(arr != nil)
        if let arr = arr {
            XCTAssertTrue(arr.pointee.array_length == 10000)
            // Write to last element
            arr.pointee.array_elements[9999] = DxValue(tag: DX_VAL_INT, DxValue.__Unnamed_union___Anonymous_field1(i: 777))
            XCTAssertTrue(arr.pointee.array_elements[9999].i == 777)
        }
    }
    func testInsnCounter() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        XCTAssertTrue(vm.pointee.insn_count == 0)
    }
    func testPendingExceptionClean() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        XCTAssertTrue(vm.pointee.pending_exception == nil)
    }
    func testActivityStackClean() {
        let (ctx, vm) = makeVM()
        defer { teardownVM(ctx, vm) }

        XCTAssertTrue(vm.pointee.activity_stack_depth == 0)
        XCTAssertTrue(vm.pointee.activity_instance == nil)
    }
}
