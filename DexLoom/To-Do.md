# DexLoom Master To-Do

## 1. Mission Definition

### What "Full APK Runner" Means
DexLoom aims to load Android APK files on jailed iOS and execute their Dalvik bytecode to the maximum extent technically feasible. This means:
- Parsing APK containers (ZIP with specific structure)
- Parsing AndroidManifest.xml (binary AXML format)
- Parsing resources.arsc (Android resource tables)
- Parsing and verifying DEX bytecode (Dalvik Executable format)
- Interpreting all 256 Dalvik opcodes with correct semantics
- Providing a compatibility layer for Android framework APIs (android.*, java.*, kotlin.*, androidx.*)
- Rendering Android UI layouts via SwiftUI translation
- Handling Activity lifecycle, user interaction, and state

### What "Supports Every Single APK" Would Imply
True universal support would require:
- A complete Java SE runtime library implementation
- A complete Android framework reimplementation (millions of lines of code in AOSP)
- JNI bridge capable of executing ARM ELF shared libraries (.so files)
- Binder IPC emulation for system services
- Linux kernel syscall translation layer
- OpenGL ES / Vulkan GPU pipeline translation
- Full threading with Java Memory Model semantics
- Content provider, broadcast receiver, and service lifecycle
- WebView (Chromium) equivalent

This is **not achievable** in full. The goal is to maximize the subset of APKs that produce meaningful output.

### Jailed iOS Constraints
- **No JIT compilation**: All code must be interpreted or ahead-of-time compiled before distribution
- **No unsigned native code execution**: Cannot dlopen arbitrary .so files extracted from APKs
- **Sandbox restrictions**: Limited filesystem access, no /proc, no ptrace, no fork
- **No background execution model matching Android**: iOS suspends apps aggressively
- **Graphics stack mismatch**: No OpenGL ES context sharing, no SurfaceFlinger, no Hardware Composer
- **No Binder IPC**: Android's fundamental IPC mechanism does not exist
- **No Linux kernel**: No /dev, /sys, /proc, no Android-specific syscalls
- **Memory pressure**: iOS kills background apps; Android expects more headroom

### What "Production-Grade" Means
- Parsers reject malformed input without crashing
- Runtime failures are contained and reported, never silently corrupt state
- Performance is acceptable for interactive use (sub-second layout inflation, <100ms UI updates)
- Test coverage exists for all critical paths
- Diagnostics allow debugging APK failures without source access
- The codebase is maintainable by engineers who did not write it

---

## 2. Current State Assessment

### Implemented (Real, Functional)
- ZIP/APK parser with STORE and DEFLATE decompression
- Binary AXML parser for manifest and layout XML
- DEX parser for all ID tables, class data, code items, encoded values
- Bytecode interpreter with all 256 Dalvik opcodes covered
- Class loading with multi-DEX support (8 DEX files), superclass chain, interface list
- VTable construction with override detection
- Mark-sweep GC with 5 root sets (activity, frames, static fields, UI tree, interned strings)
- String interning (8192 capacity)
- Try/catch exception handling with class hierarchy matching
- Per-call instruction budget (non-fatal exhaustion)
- 30+ Android view types rendered via SwiftUI
- Binary layout XML parsing with attribute extraction
- C-to-Swift bridge with render model serialization
- JNI bridge skeleton (232 function pointers, ~30 functional)
- 360+ registered Android/Java framework classes
- Real implementations: String (35+ methods), ArrayList, HashMap, StringBuilder, Activity, LayoutInflater, Enum, Math
- Exception class hierarchy (14 concrete types)
- Resources.arsc parser with string/int/bool/color/dimension/float support
- Manifest parser with package, activities, services, receivers, providers, permissions
- APKInspector CLI tool

### Partially Implemented
- [PARTIAL] Resources.arsc: No style/theme resolution, no qualifier precedence beyond layout dirs
- [DONE] Manifest: SDK versions (minSdkVersion, targetSdkVersion) and app label extracted
- [DONE] JNI bridge: Full Call*Method dispatch, Get/Set*Field, exception tracking, RegisterNatives, GetStringChars UTF-16
- [DONE] Exception propagation: Cross-method unwinding via DX_ERR_EXCEPTION + vm->pending_exception
- [DONE] DEX static values: VALUE_ARRAY and VALUE_ANNOTATION parsed (skip contents, advance pointer)
- [DONE] Reflection: Class.forName, Method.invoke, Field.get/set with real dispatch
- [DONE] SharedPreferences: In-memory key-value store with real get/put/commit/apply via Editor
- [DONE] Handler/Looper: post/postDelayed synchronous; Looper.getMainLooper/myLooper return objects; MessageQueue stubs
- [DONE] Collections: synchronizedMap/List return argument unchanged; emptyList/emptyMap return real empty objects; unmodifiableList/Map passthrough

### Simplified (Placeholder Logic That Must Be Replaced)
- [SIMPLIFIED] ConstraintLayout rendered as ZStack (no constraint solving)
- [DONE] RelativeLayout: parent-alignment flags + centering via ZStack with per-child Alignment; sibling refs parsed but not yet positioned
- [DONE] ImageView loads actual PNG/JPEG from APK res/drawable-*; falls back to placeholder
- [DONE] EditText: SwiftUI TextField with text binding back to C UI node
- [DONE] RecyclerView: Real adapter pattern with getItemCount/onCreateViewHolder/onBindViewHolder
- [SIMPLIFIED] Thread.start() runs synchronously (no concurrency)
- [SIMPLIFIED] monitor-enter/exit are no-ops (no synchronization)
- [DONE] System.currentTimeMillis() returns actual wall clock time
- [DONE] Log.d/i/w/e/v forward to DexLoom logging system (DX_DEBUG/INFO/WARN/ERROR)
- [DONE] Toast.makeText logs message + returns Toast object; show() logged
- [DONE] getSystemService returns stub objects for 10+ common services (inflater, InputMethodManager, ClipboardManager, etc.); generic non-null for unknown services

### Missing (Not Started)
- [DONE] Annotation parsing from DEX (type + visibility stored on DxClass/DxMethod)
- [DONE] Debug info parsing (line number tables via dx_parse_debug_info / dx_method_get_line)
- [MISSING] DEX verification (no bytecode verification pass)
- [MISSING] Style/theme resource resolution
- [MISSING] Resource qualifier system (locale, density, orientation, night mode, API level)
- [DONE] Drawable resource loading (PNG/JPEG from APK res/drawable-* via dx_ui_extract_drawable; ImageView renders real images)
- [DONE] java.io.* (File with path storage, FileInputStream/FileOutputStream/BufferedReader/BufferedWriter stubs)
- [PARTIAL] java.net.* (URL/HttpURLConnection/HttpsURLConnection stubs exist; Socket missing)
- [DONE] java.nio.* (ByteBuffer with field-backed storage, ByteOrder, Charset, StandardCharsets, FileChannel stub)
- [PARTIAL] android.database.* (SQLiteDatabase stub with empty Cursor, transaction support; ContentValues missing)
- [MISSING] android.webkit.* (WebView)
- [MISSING] android.media.* (MediaPlayer, AudioManager)
- [DONE] android.animation.* (ObjectAnimator, ValueAnimator, AnimatorSet, PropertyValuesHolder, ViewPropertyAnimator)
- [DONE] android.view.animation.* (AlphaAnimation, TranslateAnimation, ScaleAnimation, RotateAnimation, AnimationSet, AnimationUtils)
- [MISSING] android.graphics.Canvas drawing operations
- [MISSING] Bitmap decoding and rendering
- [DONE] Content providers: ContentProvider/ContentResolver stubs with CRUD methods
- [DONE] Broadcast receivers: registerReceiver/sendBroadcast with Intent action logging
- [DONE] Service lifecycle: startService allocates+calls onCreate/onStartCommand; IntentService subclass
- [MISSING] Dynamic proxy / Proxy.newProxyInstance
- [DONE] java.lang.reflect.Method.invoke (real dispatch via dx_vm_execute_method)
- [DONE] ClassLoader: loadClass/getParent/getResource registered
- [MISSING] Split APK / App Bundle support
- [MISSING] Jetpack Compose runtime
- [MISSING] CI/CD pipeline
- [MISSING] Fuzzing infrastructure
- [MISSING] Performance benchmarking harness

### Blocked by Platform Constraints
- [BLOCKED] Executing ARM .so native libraries (no unsigned code execution on jailed iOS)
- [BLOCKED] OpenGL ES / Vulkan rendering pipeline (no shared GPU context)
- [BLOCKED] Binder IPC (Android kernel feature)
- [BLOCKED] /proc filesystem access
- [BLOCKED] Android system services (ActivityManager, PackageManager, WindowManager as system processes)
- [BLOCKED] Background services matching Android behavior (iOS suspends)
- [BLOCKED] Camera/microphone access via Android APIs (would need iOS AVFoundation bridge)
- [BLOCKED] USB/Bluetooth/NFC hardware access via Android APIs

### Audit Required
- [AUDIT] Exact count of framework classes with real vs stub implementations
- [AUDIT] Memory leak analysis under sustained APK execution
- [AUDIT] Correctness of wide (64-bit) arithmetic edge cases (overflow, NaN propagation)
- [AUDIT] VTable correctness with diamond inheritance patterns
- [AUDIT] GC correctness with circular references
- [AUDIT] String interning memory growth under heavy load
- [AUDIT] All invoke-super resolution paths for correctness
- ~~[AUDIT] fill-array-data with 8-byte elements~~ [DONE] All element widths (1/2/4/8 bytes) supported
- [AUDIT] Endianness assumptions in DEX parsing on ARM64

---

## 3. Master Workstreams

### 3.1 Core Architecture and Repository Hygiene

#### Module Boundaries
- [ ] [HARDEN] Define clear public API surface per module (APK, DEX, VM, Runtime, UIBridge, AndroidMini)
- [x] [DONE] Move all `extern` declarations to proper header files (dx_memory.h replaces scattered externs)
- [ ] [HARDEN] Eliminate cross-module includes of .c files
- [x] [DONE] Create dx_memory.h header for dx_malloc/dx_free/dx_strdup/dx_realloc declarations
- [ ] [HARDEN] Standardize error propagation pattern (currently mix of return codes and silent absorption)

#### Symbol Naming
- [ ] All public C symbols use `dx_` prefix consistently
- [ ] All static functions use module-specific prefix or are truly static
- [ ] Eliminate naming collisions between `insn_count` (try_item struct field) and `vm->insn_count`

#### Build System
- [ ] [MISSING] Add build configuration for Release vs Debug (currently only Debug)
- [ ] [MISSING] Add warning flags: -Wall -Wextra -Wpedantic -Werror for C files
- [ ] [MISSING] Strip debug symbols in Release configuration
- [ ] [MISSING] Add static analysis (clang-tidy or similar) to build
- [ ] Verify all .c files compile cleanly with -Wconversion

#### Documentation
- [ ] [PARTIAL] Update Docs/DEXSupportMatrix.md (claims 35 opcodes; actual is 197+)
- [ ] [PARTIAL] Update Docs/Roadmap.md (many listed non-support items are now implemented)
- [ ] [PARTIAL] Update Docs/AndroidMiniAPI.md (severely outdated; lists ~10 classes, actual is 300+)
- [ ] [MISSING] API documentation for dx_jni.h public interface
- [ ] [MISSING] Architecture diagram reflecting current state (multi-DEX, JNI, GC)
- [ ] [MISSING] Troubleshooting guide for common APK failure patterns

#### CI/CD
- [ ] [MISSING] GitHub Actions workflow for build validation on push
- [ ] [MISSING] Automated test execution in CI
- [ ] [MISSING] Build for all supported simulator targets
- [ ] [MISSING] Artifact archiving for release builds

#### Technical Debt
- [ ] Remove pointer-cast abuse for string storage (fields[0].obj = (DxObject*)(uintptr_t)str) — use dedicated string_data field on DxObject
- [x] Replace O(n) linear class lookup with FNV-1a hash table (4096-entry open addressing)
- [x] Replace HashMap/ArrayList with real array-backed storage (put/get/remove/contains work)
- [x] DX_MAX_HEAP_OBJECTS consistent at 65536 (no discrepancy)
- [x] DX_MAX_INSTRUCTIONS consistent at 500000 (no discrepancy)

---

### 3.2 APK Container Support

#### Current: 95% Complete

- [x] Central directory parsing with backward EOCD search
- [x] Local file header validation
- [x] STORE (method 0) and DEFLATE (method 8) decompression
- [x] Entry iteration and filename matching
- [x] Bounds checking on all reads
- [x] Two-pass EOCD search (strict then lenient)

#### Remaining Work
- [ ] [MISSING] ZIP64 extension support (archives >4GB or >65535 entries)
- [ ] [MISSING] Encrypted entry detection and clear error reporting
- [ ] [MISSING] Streaming extraction for large entries (currently loads entire entry to memory)
- [ ] [HARDEN] Validate local file header CRC32 against central directory
- [ ] [HARDEN] Detect and reject zip bombs (compression ratio > 100:1)
- [ ] [HARDEN] Path traversal prevention in entry filenames (../ sequences)
- [ ] [OPTIMIZE] Memory-mapped file access instead of full file read for large APKs
- [ ] [MISSING] Split APK support (base.apk + split_config.*.apk)
- [ ] [MISSING] App Bundle (.aab) support or conversion
- [ ] [MISSING] APK signature block parsing (V2/V3 signature detection for integrity)

---

### 3.3 Android Binary XML (AXML) and Manifest Support

#### Current: 75% Complete

- [x] Binary XML magic validation and chunk walking
- [x] String pool parsing (UTF-8 and UTF-16)
- [x] Resource ID map parsing
- [x] START_TAG / END_TAG / TEXT chunk handling
- [x] Package name extraction
- [x] Main activity detection via MAIN+LAUNCHER intent filter
- [x] Permission list extraction
- [x] Activity/Service/Receiver/Provider enumeration

#### Remaining Work
- [x] [DONE] Extract minSdkVersion and targetSdkVersion from uses-sdk tag
- [x] [DONE] Extract android:label from application tag
- [x] [DONE] Extract android:theme attribute (resource reference or string)
- [x] [DONE] Parse intent-filter action/category/data details for all components (DxIntentFilter with arrays)
- [x] [DONE] Parse `<meta-data>` tags (name+value key-value pairs on components and application)
- [x] [DONE] Parse `<uses-feature>` tags (name + required flag)
- [x] [DONE] Parse `<uses-library>` tags (name + required flag)
- [ ] [MISSING] Namespace-aware attribute resolution (currently uses resource ID matching)
- [x] [DONE] Support for multiple intent filters per component (dynamic array on DxComponent)
- [ ] [HARDEN] Malformed chunk recovery (truncated attributes, invalid string refs)
- [ ] [HARDEN] Maximum nesting depth check to prevent stack overflow on deeply nested XML
- [ ] [MISSING] Full UTF-16 support (currently ASCII-only for UTF-16 strings: non-ASCII becomes '?')
- [x] [DONE] Extract `android:exported` flag (with exported_set to distinguish absent vs false)
- [ ] [MISSING] Parse `<instrumentation>` tags

---

### 3.4 Resources.arsc and Android Resources System

#### Current: 90% Complete for Parsing, 10% for Resolution

- [x] ResTable header parsing
- [x] String pool decoding (UTF-8 and UTF-16)
- [x] Package chunk walking
- [x] Type spec and type chunks
- [x] Entry extraction with value decoding
- [x] Resource ID → value lookup (dx_resources_find_by_id)
- [x] Resource name lookup (dx_resources_find_by_name)
- [x] Value types: string, int_dec, int_hex, int_boolean, float
- [x] Color formats: ARGB8, RGB8, ARGB4, RGB4 with normalization
- [x] Dimension decoding with units (px, dp, sp, pt, in, mm)
- [x] Reference type detection

#### Remaining Work: Resource Resolution
- [x] [DONE] **Style resource resolution** — ResTable_map_entry parsing, parent chain traversal (20 levels), child-overrides-parent
  - [x] Parse style parent references
  - [x] Resolve style attribute inheritance chains
  - [x] Apply style attributes to views during inflation (as defaults, direct attrs override)
- [x] [DONE] **Theme resolution** — android:theme on Application extracted from manifest, DxTheme created at runtime
  - [x] Parse theme attributes from resources.arsc (complex/bag entries)
  - [x] Apply theme defaults to unset view attributes
  - [x] ?attr/ reference resolution through theme (colorPrimary, colorAccent, windowBackground, textColorPrimary)
  - [ ] Support theme overlay composition
- [x] [DONE] **Qualifier resolution system** — DxResConfig parsed from ResTable_config; DxDeviceConfig (440dpi, en-US, SDK 33)
  - [x] Locale qualifiers (language+country matching with scoring)
  - [x] Screen density qualifiers (LDPI through XXXHDPI, closest-match with higher-preferred)
  - [x] Orientation qualifiers (port, land)
  - [x] API level qualifier (SDK version, highest <= device)
  - [x] Best-match algorithm: eliminate contradictions, score by locale>SDK>density>orientation
  - [ ] Screen size qualifiers (small, normal, large, xlarge)
  - [ ] Night mode qualifier (night, notnight)
- [x] [PARTIAL] **Drawable resources**
  - [x] PNG/JPEG decoding from res/drawable-* (via dx_ui_extract_drawable + UIImage)
  - [ ] 9-patch PNG support
  - [ ] Vector drawable (XML path data) support
  - [ ] StateListDrawable (selector XML)
  - [ ] LayerDrawable, InsetDrawable, ShapeDrawable
- [ ] [MISSING] **Array resources** (`<string-array>`, `<integer-array>`)
- [ ] [MISSING] **Plural resources** (`<plurals>`)
- [x] [DONE] **Attribute reference resolution** (?attr resolved via theme; @style applied during inflation)
- [ ] [HARDEN] Detect and handle circular style references
- [ ] [OPTIMIZE] Resource cache with LRU eviction for repeated lookups

---

### 3.5 DEX Parsing and Verification

#### Current: 85% Complete for Parsing, 0% for Verification

- [x] Header validation (magic, version, file size)
- [x] All 6 ID tables (string, type, proto, field, method, class_def)
- [x] MUTF-8 string decoding
- [x] Class data parsing with ULEB128/SLEB128
- [x] Code item parsing (registers, ins, outs, tries size, instructions)
- [x] Encoded value parsing (12 of 14 types)
- [x] Lazy class data parsing on demand
- [x] Multi-DEX loading

#### Remaining Work: Parsing
- [x] [DONE] Encoded VALUE_ARRAY parsing (skip contents but advance pointer correctly)
- [x] [DONE] Encoded VALUE_ANNOTATION parsing (skip contents but advance pointer correctly)
- [x] [DONE] **Annotation parsing** — annotations parsed and stored on DxClass/DxMethod
  - [x] Parse annotation_set_item
  - [x] Parse annotation_item
  - [x] Parse encoded_annotation
  - [x] Store annotations on DxClass and DxMethod (type + visibility)
  - [ ] Required for: Retrofit (@GET, @POST), Room (@Entity, @Dao), Dagger (@Inject) — annotation values not yet extracted
- [x] [DONE] **Debug info parsing**
  - [x] Parse debug_info_item (special opcode state machine)
  - [x] Extract line number table (binary search lookup via dx_method_get_line)
  - [ ] Extract local variable names (for debugging)
  - [ ] Extract parameter names
- [ ] [MISSING] **Map section parsing** (map_list for structural validation)
- [ ] [MISSING] **Hidden API metadata** (DEX 039+ greylist/blacklist flags)
- [ ] [HARDEN] Full checksum validation (Adler32)
- [ ] [HARDEN] SHA-1 signature validation

#### Remaining Work: Verification
- [ ] [MISSING] **Bytecode verifier** — a critical missing subsystem
  - [ ] Register type tracking through control flow
  - [ ] Verify method signatures match call sites
  - [ ] Verify field types match access patterns
  - [ ] Verify branch targets are valid instruction boundaries
  - [ ] Verify exception handler ranges
  - [ ] Verify array access type safety
  - [ ] Verify const-string indices are in bounds
  - [ ] Verify class/method/field indices are in bounds
  - [ ] Report verification failures with actionable diagnostics
- [ ] [MISSING] Malformed DEX detection and rejection (currently trusts input)
- [ ] [HARDEN] Bounds check on all code[pc] reads (currently trusts insns_size)

---

### 3.6 Java Runtime / Language Runtime Support

#### Object Model
- [x] DxObject with class pointer, field array, GC mark
- [x] DxClass with descriptor, superclass, methods, fields, vtable, interfaces
- [x] DxValue tagged union (void, int, long, float, double, obj)
- [x] Array objects with typed elements
- [x] [DONE] Proper null type handling (instance-of returns 0 for null, check-cast succeeds for null)
- [x] [DONE] Object.getClass() returning proper java.lang.Class object
- [x] [DONE] Object.wait()/notify()/notifyAll() (no-op in single-threaded model)

#### Class Loading
- [x] Multi-DEX class search
- [x] Superclass chain loading
- [x] Interface list parsing
- [x] Circular dependency detection (64-depth loading stack)
- [x] Suffix match fallback for obfuscated names
- [ ] [MISSING] Custom ClassLoader support (many frameworks use custom loaders)
- [ ] [MISSING] Class.forName with initialization flag
- [ ] [MISSING] Class unloading
- [ ] [HARDEN] Detect and report class version conflicts across DEX files

#### Inheritance and Interfaces
- [x] Single inheritance via superclass chain
- [x] Interface list stored per class
- [x] instance-of checks interfaces array
- [ ] [MISSING] Interface method table (itable) for efficient interface dispatch
- [ ] [MISSING] Default interface methods (Java 8+)
- [ ] [MISSING] Multiple interface inheritance diamond resolution

#### Method Dispatch
- [x] invoke-virtual with vtable dispatch
- [x] invoke-super with superclass resolution
- [x] invoke-direct (constructors, private methods)
- [x] invoke-static
- [x] invoke-interface
- [x] All /range variants
- [ ] [PARTIAL] invoke-super fallback: searches by name+shorty then vtable_idx; may fail on complex hierarchies
- [ ] [MISSING] Bridge method handling (compiler-generated type erasure bridges)
- [x] [DONE] Varargs method invocation (pack_varargs packs trailing args into Object[] array)

#### Exceptions
- [x] throw opcode with catch handler search
- [x] Typed catch with class hierarchy matching
- [x] Catch-all handler support
- [x] 14 concrete exception classes with proper hierarchy
- [x] dx_vm_create_exception() for typed exceptions
- [x] ArithmeticException on div-by-zero
- [x] [DONE] **Exception propagation**: cross-method unwinding via DX_ERR_EXCEPTION + vm->pending_exception. Throw in callee propagates to caller's catch handlers up the full call stack.
  - [x] Call-stack unwinding implemented in handle_invoke/handle_invoke_range
  - [x] Top-level calls (init, onCreate, onClick, fragment lifecycle) absorb uncaught exceptions gracefully
- [x] [DONE] NullPointerException on null dereference (iget on null, aget on null array)
- [x] [DONE] ArrayIndexOutOfBoundsException on array access (aget out of bounds)
- [x] [DONE] ClassCastException on failed check-cast (with class hierarchy + interface checking)
- [x] [DONE] finally block support (catch_all handlers execute on normal return and exception paths)
- [x] [PARTIAL] Exception chaining (getCause() done; initCause() not yet implemented)
- [x] [DONE] Stack trace generation (debug info parsed; getMessage/toString/getCause/printStackTrace/getStackTrace implemented)

#### Reflection
- [x] Class.forName with descriptor conversion
- [x] Class.isAssignableFrom
- [x] [DONE] **java.lang.reflect.Method**: getDeclaredMethod, invoke with real dispatch
- [x] [DONE] **java.lang.reflect.Field**: getDeclaredField, get, set with real field access
- [x] [DONE] **java.lang.reflect.Constructor**: Class.newInstance() allocates and calls <init>
- [x] [DONE] Method.getAnnotation / Class.getAnnotation / isAnnotationPresent / getAnnotations — walks DxClass.annotations[]
- [x] [DONE] Proxy.newProxyInstance — runtime DxClass generation, delegates to InvocationHandler.invoke
- [x] [DONE] Array.newInstance / getLength / get / set for reflective array operations
- [x] [DONE] Constructor class: Class.getConstructor/getDeclaredConstructor, Constructor.newInstance
- [x] [DONE] Class.getDeclaredMethods() / getDeclaredFields() returning real arrays

#### Garbage Collection
- [x] Mark-sweep with 5 root sets
- [x] Triggers at 80% heap capacity
- [x] Compaction (shifts heap entries to fill gaps)
- [ ] [OPTIMIZE] Incremental GC to avoid long pauses
- [ ] [OPTIMIZE] Generational collection (young/old generations)
- [ ] [HARDEN] GC correctness with weak references
- [ ] [AUDIT] Verify no dangling pointers after sweep (especially in frame registers)
- [x] [DONE] WeakReference, SoftReference extend Reference; ReferenceQueue stub; get/clear/enqueue

#### Threading
- [x] Thread.start() calls run() synchronously
- [ ] [LIMITED] **True threading is impractical on single-threaded interpreter** but important for correctness:
  - [ ] Implement cooperative threading with round-robin scheduling at interpreter level
  - [ ] Or: detect Thread.start() and log warning about synchronous execution
  - [ ] synchronized blocks should at minimum track ownership (prevent self-deadlock detection)
- [x] [DONE] ExecutorService, ThreadPoolExecutor (execute runs synchronously)
- [x] [PARTIAL] java.util.concurrent.* (AtomicInteger/Boolean/Reference, ReentrantLock, CountDownLatch, Semaphore done; concurrent collections missing)
- [ ] [MISSING] volatile field semantics
- [x] [DONE] CountDownLatch, Semaphore (stubs with countDown/await/acquire/release)

---

### 3.7 Bytecode Interpreter / Execution Engine

#### Current: All 256 Dalvik opcodes covered (stubs for polymorphic/custom)

#### Opcode Coverage Matrix

| Family | Opcodes | Status | Notes |
|--------|---------|--------|-------|
| nop | 0x00 | Real | |
| move | 0x01-0x09 | Real | All widths including /16 and wide |
| move-result | 0x0A-0x0D | Real | Including move-exception |
| return | 0x0E-0x11 | Real | void, int, wide, object |
| const | 0x12-0x1B | Real | All variants including const-wide (5 units) |
| const-string | 0x1A-0x1B | Real | Including /jumbo |
| const-class | 0x1C | Real | |
| monitor | 0x1D-0x1E | **Stub** | No threading model |
| check-cast | 0x1F | Real | |
| instance-of | 0x20 | Real | With interface checking |
| array-length | 0x21 | Real | |
| new-instance | 0x22 | Real | |
| new-array | 0x23 | Real | |
| filled-new-array | 0x24-0x25 | Real | 35c and 3rc |
| fill-array-data | 0x26 | Real | All element widths (1/2/4/8 bytes) |
| throw | 0x27 | Real | With catch handler search |
| goto | 0x28-0x2A | Real | 8/16/32 bit offsets |
| switch | 0x2B-0x2C | Real | packed and sparse |
| cmp | 0x2D-0x31 | Real | float/double/long with NaN semantics |
| if | 0x32-0x3D | Real | All 12 variants |
| aget | 0x44-0x4A | Real | All 7 type variants |
| aput | 0x4B-0x51 | Real | All 7 type variants |
| iget | 0x52-0x58 | Real | All 7 type variants |
| iput | 0x59-0x5F | Real | All 7 type variants |
| sget | 0x60-0x66 | Real | All 7 type variants |
| sput | 0x67-0x6D | Real | All 7 type variants |
| invoke | 0x6E-0x72 | Real | virtual/super/direct/static/interface |
| invoke/range | 0x74-0x78 | Real | All 5 variants |
| unary | 0x7B-0x8F | Real | neg, not, conversions |
| binop | 0x90-0xAF | Real | add/sub/mul/div/rem/and/or/xor/shl/shr/ushr |
| binop/2addr | 0xB0-0xCF | Real | All variants |
| binop/lit16 | 0xD0-0xD7 | Real | |
| binop/lit8 | 0xD8-0xE2 | Real | |
| invoke-polymorphic | 0xFA-0xFB | **Stub** | Returns null with warning |
| invoke-custom | 0xFC-0xFD | **Stub** | Returns null with warning |
| const-method-handle | 0xFE | **Stub** | Stores null |
| const-method-type | 0xFF | **Stub** | Stores null |
| reserved | 0x3E-0x43, 0x73, 0x79-0x7A, 0xE3-0xF9 | Default | Skipped by width |

#### Remaining Work
- [x] [DONE] fill-array-data: 8-byte (long/double) element support added
- [ ] [MISSING] **invoke-custom real implementation** — required for Java 8+ lambdas not desugared by R8
  - [ ] Parse call_site_item from DEX
  - [ ] Implement bootstrap method invocation
  - [ ] Handle StringConcatFactory (string concatenation in Java 9+)
  - [ ] Handle LambdaMetafactory (lambda expressions)
- [ ] [MISSING] **invoke-polymorphic** — required for MethodHandle.invoke/invokeExact
- [ ] [MISSING] const-method-handle / const-method-type real values
- [x] [DONE] Bounds check via CODE_AT() macro for safe code access
- [x] [DONE] Validate register indices via CHECK_REG macro on critical opcodes
- [x] [DONE] Integer overflow protection in div/rem (INT_MIN / -1 returns INT_MIN, INT_MIN % -1 returns 0)
- [ ] [OPTIMIZE] Computed goto dispatch (gcc/clang extension) instead of switch
- [ ] [OPTIMIZE] Register caching (avoid DxValue indirection for hot registers)
- [ ] [OPTIMIZE] Inline caching for invoke-virtual (monomorphic/polymorphic)
- [ ] [MISSING] Trace logging toggle per method (current TRACE level floods output)

#### Exception Flow (Critical Fix)
- [x] **[DONE] Call-stack exception unwinding implemented**
  - DX_ERR_EXCEPTION returned from dx_vm_execute_method with vm->pending_exception set
  - handle_invoke/handle_invoke_range search caller's try/catch handlers
  - Unhandled exceptions propagate up the full call stack
  - Top-level entry points (init, onCreate, onClick, fragment lifecycle) absorb uncaught exceptions

---

### 3.8 Android Framework Compatibility Layer

#### Implementation Tiers

**Tier A: Mandatory for Basic APK Support (many already implemented)**

| Package | Class | Status | Notes |
|---------|-------|--------|-------|
| android.app | Activity | Partial | setContentView, findViewById work; lifecycle stubs |
| android.app | Application | Stub | onCreate noop |
| android.content | Context | Stub | getSystemService partial (inflater only) |
| android.content | Intent | Stub | All getters return defaults |
| android.os | Bundle | Stub | All getters return defaults |
| android.os | Handler | Stub | post/postDelayed are noops |
| android.view | View | Partial | setOnClickListener works; most setters noop |
| android.view | ViewGroup | Stub | addView/removeView noop |
| android.view | LayoutInflater | Real | inflate() parses XML layouts |
| android.widget | TextView | Partial | setText/getText work via UI node |
| android.widget | Button | Partial | Inherits TextView; click works |
| android.widget | LinearLayout | Stub | Orientation in XML only |
| android.widget | FrameLayout | Stub | |
| android.widget | ScrollView | Stub | |
| android.content.res | Resources | Stub | getString returns null |
| java.lang | String | Real | 25 methods |
| java.lang | Object | Real | equals, hashCode, toString |
| java.lang | System | Partial | arraycopy real; currentTimeMillis returns 0 |
| java.util | ArrayList | Real | Full implementation |
| java.util | HashMap | Real | O(n) but functional |

**Remaining Tier A Work:**
- [x] Resources.getString(int id) uses resources.arsc lookup via dx_resources_get_string_by_id
- [x] Context.getResources() returns real Resources object
- [x] Context.getPackageName() returns manifest package name
- [x] System.currentTimeMillis() returns actual wall clock time
- [x] Handler.post() executes Runnable.run() synchronously
- [x] Handler.postDelayed() executes Runnable.run() synchronously (delay ignored)
- [x] Log.d/i/w/e/v forward to DexLoom DX_DEBUG/INFO/WARN/ERROR logging
- [x] Toast.makeText logs message + returns Toast object; show() is no-op

**Tier B: Needed for Broader Compatibility**

- [x] [DONE] **android.content.SharedPreferences** — in-memory key/value store
  - [x] getString, getInt, getBoolean, getFloat, getLong with defaults
  - [x] Editor with putString, putInt, putBoolean, putLong, putFloat, commit, apply
- [x] [DONE] **android.widget.RecyclerView** — real adapter pattern
  - [x] Adapter.getItemCount, onCreateViewHolder, onBindViewHolder
  - [x] ViewHolder pattern with itemView
  - [x] Basic linear layout manager + GridLayoutManager
- [x] [DONE] **android.widget.ListView / GridView** — legacy list patterns
  - [x] ArrayAdapter, BaseAdapter with real adapter dispatch
  - [x] getView, getCount, getItem + AbsListView hierarchy
- [x] [DONE] **android.app.AlertDialog + Builder** — builder pattern with chaining
  - [x] setTitle, setMessage, setPositiveButton, setNegativeButton, setView, etc.
  - [x] create() returns AlertDialog; show() simulated (logged)
- [x] [DONE] **android.graphics.Bitmap / BitmapFactory**
  - [x] BitmapFactory.decodeResource, decodeStream, decodeByteArray (return stub 100x100)
  - [x] Bitmap.createBitmap, getWidth, getHeight (field-backed)
  - [ ] Bridge to UIImage / CGImage (deferred)
- [x] [DONE] **android.graphics.Canvas** — stub with all draw methods (no Core Graphics bridge)
  - [x] drawRect, drawCircle, drawLine, drawText, drawBitmap, drawColor, save/restore/translate/rotate/scale
  - [ ] Bridge to Core Graphics (deferred)
- [x] [DONE] **java.io.File** — real init/path storage, simulated filesystem ops
  - [x] exists, mkdir, mkdirs, delete, isDirectory, getName, getPath, getAbsolutePath, length
  - [x] File(String) and File(File, String) constructors store path
- [x] [DONE] **java.io.FileInputStream / FileOutputStream** — stub classes registered
- [x] [DONE] **java.io.BufferedReader / BufferedWriter / PrintWriter** — stub classes
- [x] [DONE] **android.net.Uri** — parse() creates Uri object, toString() returns stored string
- [x] [DONE] **android.text.TextUtils** — isEmpty, equals, join, isDigitsOnly, htmlEncode, getTrimmedLength, concat
- [x] [DONE] **android.util.TypedValue** — applyDimension, complexToDimensionPixelSize
- [x] [DONE] **android.util.SparseArray / SparseBooleanArray / SparseIntArray** — registered with stubs

**Tier C: Needed for Near-Universal Compatibility**

- [x] [DONE] **android.database.sqlite.SQLiteDatabase** — stub with simulated queries, empty Cursor, transaction support
- [x] [DONE] **android.webkit.WebView** — WKWebView bridge with loadUrl/loadData/loadDataWithBaseURL; WebSettings (14 methods), WebViewClient, WebChromeClient
- [x] [DONE] **java.net.HttpURLConnection** — stub with URL storage, simulated 200 response, getInputStream/getOutputStream
- [x] [DONE] **java.net.URL / URI** — constructor, toString, openConnection, create
- [x] [DONE] **javax.net.ssl.HttpsURLConnection** — extends HttpURLConnection + SSL stubs
- [x] [DONE] **android.media.MediaPlayer** — 25 methods (create/setDataSource/start/pause/stop/release/seekTo/setVolume/listeners); SoundPool, AudioAttributes, AudioManager (16 constants), AudioFocusRequest, Ringtone/RingtoneManager, AudioFormat
- [ ] [MISSING] **android.location.LocationManager** — bridge to CLLocationManager
- [x] [DONE] **android.content.ContentResolver** — stub CRUD methods (query/insert/update/delete)
- [x] [DONE] **android.content.BroadcastReceiver** — registered with onReceive stub
- [x] [DONE] **android.app.Service** — registered with lifecycle stubs + startForeground
- [x] [DONE] **android.app.NotificationManager** — notify/cancel/createNotificationChannel stubs
- [x] [DONE] **android.view.animation.\*** — Animation, AlphaAnimation, TranslateAnimation, ScaleAnimation, RotateAnimation, AnimationSet, AnimationUtils
- [x] [DONE] **android.animation.\*** — ValueAnimator, ObjectAnimator, AnimatorSet, PropertyValuesHolder, ViewPropertyAnimator
- [x] [DONE] **Jetpack lifecycle components** — LiveData, MutableLiveData (with observer dispatch), ViewModel, ViewModelProvider
- [x] [DONE] **Jetpack Navigation** — NavController (navigate/popBackStack), NavHostFragment

**Tier D: Likely Impossible or Extremely Constrained on Jailed iOS**

- [ ] [BLOCKED] android.hardware.Camera / Camera2 — requires AVCaptureSession bridge
- [ ] [BLOCKED] android.bluetooth.* — requires CoreBluetooth bridge and entitlements
- [ ] [BLOCKED] android.nfc.* — no NFC API on iOS for third-party apps
- [ ] [BLOCKED] android.telephony.* — no telephony API access
- [ ] [BLOCKED] android.provider.ContactsContract — requires Contacts framework bridge
- [ ] [BLOCKED] android.provider.MediaStore — requires Photos framework bridge
- [ ] [BLOCKED] com.google.android.gms.* — Google Play Services (proprietary, binary-only)
- [ ] [BLOCKED] Jetpack Compose — requires Compose compiler plugin runtime
- [ ] [RESEARCH] android.opengl.* — could potentially bridge to Metal via MoltenVK for simple cases

---

### 3.9 Native Library / JNI Support

#### Current: JNI Bridge Skeleton Complete, No .so Loading

- [x] JNIEnv function table (232 entries)
- [x] FindClass, GetSuperclass, IsAssignableFrom (functional)
- [x] NewStringUTF, GetStringUTFChars (functional)
- [x] GetObjectClass, IsInstanceOf (functional)
- [x] GetMethodID, CallObjectMethod, CallVoidMethod (functional)
- [x] GetFieldID, GetObjectField, SetObjectField (functional)
- [x] Array creation and element access (functional)
- [x] Reference management pass-through (no ref counting needed)
- [x] RegisterNatives (logs but doesn't bind)

#### Remaining Work: JNI Bridge Improvements
- [x] [DONE] Primitive Call*Method (CallIntMethod, CallBooleanMethod, CallLongMethod) — dispatch to dx_vm_execute_method
- [x] [DONE] GetIntField, GetBooleanField, GetObjectField — use dx_vm_get_field
- [x] [DONE] SetIntField, SetBooleanField, SetObjectField — use dx_vm_set_field
- [x] [DONE] Static field Get/Set — GetStaticObjectField, SetStaticObjectField, GetStaticFieldID
- [x] [DONE] NewObject calls <init> constructor after allocation
- [x] [DONE] Exception pending state tracking (Throw, ThrowNew, ExceptionOccurred, ExceptionCheck, ExceptionClear all functional)
- [x] [DONE] RegisterNatives binds function pointers to DxMethod.native_fn
- [x] [DONE] GetStringChars (UTF-16) — proper UTF-8 → UTF-16 conversion with surrogate pairs

#### .so Loading Feasibility on Jailed iOS: [BLOCKED]
- **Cannot dlopen arbitrary ARM ELF binaries on jailed iOS**
- **Cannot execute unsigned native code**
- Possible approaches (all [RESEARCH]):
  - [ ] [RESEARCH] Pre-translated .so → C source → recompile as iOS framework (impractical for most apps)
  - [ ] [RESEARCH] ARM interpreter (Unicorn/QEMU usermode) for simple native methods — extreme performance cost
  - [ ] [RESEARCH] Stub common native libraries (libsqlite, libz, etc.) with iOS equivalents
  - [ ] [RESEARCH] Detect JNI-only apps and report clearly: "This APK requires native code execution which is not possible on iOS"
- **Practical recommendation**: Focus on JNI bridge for DEX-side calls; treat .so as unsupported with clear error reporting

---

### 3.10 Android System Behavior Emulation

#### Binder / IPC
- [ ] [BLOCKED] Full Binder IPC is a Linux kernel feature; cannot be replicated
- [ ] [LIMITED] For getSystemService(), return stub objects that provide safe defaults
- [ ] [LIMITED] For startActivity(Intent), intercept and log the intent; possibly show target activity if in same APK

#### Looper / Handler / MessageQueue
- [x] [DONE] **Looper.prepare() / Looper.loop() / Looper.myLooper()** — no-op/return Looper object
  - [x] getMainLooper() returns real Looper object (not null)
  - [x] myLooper() returns same as getMainLooper()
  - [x] prepare()/loop()/quit()/quitSafely() are no-ops
  - [x] Handler.post() executes Runnable.run() synchronously
  - [x] Handler.postDelayed() same (delay ignored)
  - [x] MessageQueue.addIdleHandler/removeIdleHandler are no-ops
- [x] [DONE] **HandlerThread** — start/getLooper/quit/quitSafely

#### Filesystem Layout
- [x] [DONE] Map Context.getFilesDir() to "/data/data/app/files" (simulated path)
- [x] [DONE] Map Context.getCacheDir() to "/data/data/app/cache" (simulated path)
- [x] [DONE] Map Context.getExternalFilesDir(type) to /sdcard/Android/data/app/files/<type>
- [x] [DONE] Environment.getExternalStorageDirectory/getDataDirectory/getExternalStorageState/isExternalStorageEmulated
- [ ] [MISSING] Reject or sandbox /proc, /sys, /dev accesses

#### Permission Model
- [x] [DONE] checkSelfPermission returns GRANTED for safe perms (INTERNET, VIBRATE, etc.), DENIED for dangerous (CAMERA, LOCATION, etc.)
- [x] [DONE] requestPermissions() calls onRequestPermissionsResult with all-GRANTED array
- [x] [DONE] ActivityCompat/ContextCompat.checkSelfPermission + requestPermissions
- [x] [DONE] PackageManager.PERMISSION_GRANTED/DENIED constants; Manifest.permission string constants (16 common perms)

#### Process Model
- [x] [DONE] android.os.Process.myPid()=1000, myUid()=10000, myTid()=1000

---

### 3.11 UI Compatibility and Rendering

#### Current: 30+ View Types with Basic Styling

- [x] LinearLayout (VStack/HStack)
- [x] FrameLayout, RelativeLayout, ConstraintLayout (ZStack)
- [x] ScrollView
- [x] CardView (rounded corners, shadow)
- [x] TextView, Button, EditText, ImageView
- [x] Switch, CheckBox, ProgressBar, Toolbar
- [x] RecyclerView (static VStack)
- [x] View (spacer), ViewGroup (generic)
- [x] Style properties: width, height, weight, padding, margin, bgColor, textColor, textSize, gravity, hint, text, isChecked, visibility

#### Remaining Work: Layout
- [x] [DONE] **ConstraintLayout basic constraint solving** — GeometryReader-based positioning:
  - [x] layout_constraintLeft/Right/Top/Bottom_toLeftOf/toRightOf/toTopOf/toBottomOf (12 attrs)
  - [x] layout_constraintStart/End variants
  - [x] Parent anchoring + sibling anchoring (single-pass with 1-level recursion)
  - [x] Bias (horizontal_bias, vertical_bias) with default 0.5
  - [x] Stretching for 0dp/match_constraint children
  - [ ] Guidelines (horizontal/vertical)
  - [ ] Chains (spread, packed, weighted)
- [x] [DONE] **RelativeLayout positioning rules**
  - [ ] layout_above, layout_below, layout_toLeftOf, layout_toRightOf (parsed but not positioned)
  - [x] layout_alignParentTop/Bottom/Left/Right
  - [x] layout_centerInParent, layout_centerHorizontal, layout_centerVertical
- [x] [DONE] Individual padding sides (paddingLeft/Top/Right/Bottom, paddingStart/End via dx_ui_decode_dimension)
- [x] [DONE] Proper dimension unit conversion (dp/sp/px→points via dx_ui_decode_dimension; paddingStart/End, marginStart/End)
- [ ] [MISSING] match_parent / wrap_content in measure/layout cycle (currently heuristic)

#### Remaining Work: View Features
- [x] [DONE] **EditText input** — SwiftUI TextField with text binding
  - [x] Bridge to SwiftUI TextField
  - [x] Handle onTextChanged callbacks (via dx_runtime_update_edit_text)
  - [ ] Input type filtering (number, email, password)
- [x] [DONE] **ImageView actual images**
  - [x] Load from res/drawable-* in APK
  - [x] Decode PNG/JPEG via iOS UIImage
  - [ ] ScaleType support (centerCrop, fitCenter, fitXY)
- [x] [DONE] **RecyclerView with adapter**
  - [x] Call adapter methods to get item count and bind views (clamped to 50 items)
  - [x] Scroll and recycle (renders all items, no recycling)
- [x] [DONE] **Spinner (dropdown)** — rendered as styled dropdown selector
- [x] [DONE] **SeekBar / RatingBar** — rendered as SwiftUI approximations
- [x] [DONE] **TabLayout / ViewPager** — TabLayout as horizontal scroll, ViewPager shows first page
- [x] [DONE] **FloatingActionButton** — rendered as circular button with shadow
- [x] [DONE] **BottomNavigationView / Chip / RadioButton / RadioGroup / WebView placeholder**
- [x] [DONE] **AlertDialog.Builder** chaining + create/show; presentation simulated (logged)
- [x] [DONE] **Menu / options menu / context menu** — Menu/MenuItem/SubMenu/MenuInflater; Activity onCreateOptionsMenu/onOptionsItemSelected
- [x] [DONE] **Fragment UI lifecycle** (onCreateView → onViewCreated → onStart → onResume)

#### Rendering Pipeline
- [ ] [MISSING] Proper measure/layout pass (Android's 2-pass measure → layout → draw)
- [ ] [MISSING] View invalidation and partial re-render
- [ ] [MISSING] Animation support (property animations on view attributes)
- [x] [DONE] Touch event dispatch: onTapGesture on all views with click listeners, long-press, SwipeRefreshLayout pull-to-refresh
- [x] [DONE] Scroll event handling (ScrollView already renders via SwiftUI ScrollView)
- [ ] [MISSING] Focus management
- [ ] [OPTIMIZE] Diff-based UI updates (currently rebuilds entire render model)

---

### 3.12 App Lifecycle and Multitasking Model

- [x] Activity.onCreate called with null Bundle
- [x] Activity.<init> called before onCreate
- [x] Fragment.onCreateView lifecycle with R8 deobfuscation
- [x] [DONE] **Full Activity lifecycle**: onCreate→onPostCreate→onStart→onResume→onPostResume + teardown on nav (onPause→onStop→onDestroy)
- [x] [DONE] **Activity result model**: startActivityForResult/setResult/finish with back-stack, onActivityResult delivery
- [x] [DONE] **Configuration class**: android.content.res.Configuration with orientation, screenWidthDp, screenHeightDp, locale, densityDpi; Resources.getConfiguration()
- [x] [DONE] **State save/restore**: onSaveInstanceState/onRestoreInstanceState with real Bundle; Activity.recreate() with full teardown+rebuild
- [x] [DONE] **Back button handling**: onBackPressed via dx_runtime_dispatch_back
- [x] [DONE] **Multiple activities**: Intent with extras, startActivity loads target class and runs lifecycle
- [ ] [MISSING] **Task/back stack model**
- [ ] [LIMITED] **Background/foreground transitions**: iOS lifecycle doesn't match Android's

---

### 3.13 File System and Storage Compatibility

- [x] [DONE] **App-private files directory**: getFilesDir/getCacheDir return simulated paths
- [x] [DONE] **java.io.File**: init, exists, isDirectory, mkdirs, delete, getName, getPath, getAbsolutePath, length
- [x] [DONE] **java.io.FileInputStream / FileOutputStream**: stub classes with read/write/close
- [x] [DONE] **java.io.BufferedReader / BufferedWriter**: stub classes with readLine/write/close
- [x] [DONE] **java.io.InputStreamReader / OutputStreamWriter**: stub classes
- [x] [DONE] **SharedPreferences persistence**: in-memory store (256 entries, not persisted to disk)
- [x] [DONE] **Context.openFileInput / openFileOutput** — returns FileInputStream/FileOutputStream
- [x] [DONE] **Asset access**: Context.getAssets().open("filename") — extracts from APK via dx_apk_extract_entry
- [x] [DONE] **Cache directory management**: getCacheDir/getFilesDir return File objects
- [x] [DONE] **Temp file creation**: File.createTempFile with unique counter-based paths
- [ ] [HARDEN] Path traversal prevention in all file operations

---

### 3.14 Networking Compatibility

- [x] [DONE] **java.net.URL**: constructor stores URL, toString, openConnection returns HttpURLConnection
- [x] [PARTIAL] **java.net.HttpURLConnection**: stub with URL storage, simulated 200 response
  - [ ] Real URLSession bridge for GET, POST, PUT, DELETE
  - [ ] Request headers, response headers
  - [ ] Real response code, response body as InputStream
  - [ ] Follow redirects
- [x] [DONE] **javax.net.ssl.HttpsURLConnection**: extends HttpURLConnection + SSL stubs
- [ ] [MISSING] **java.net.Socket / ServerSocket**: TCP
- [ ] [MISSING] **OkHttp integration**: stubs registered but non-functional
- [ ] [MISSING] **Retrofit integration**: depends on OkHttp + reflection (annotation-based)
- [ ] [LIMITED] ClearText traffic policy (Android blocks cleartext by default in recent versions)
- [ ] [LIMITED] Certificate pinning emulation

---

### 3.15 Multimedia and Graphics

- [x] [DONE] **Bitmap decoding**: PNG/JPEG from APK via dx_ui_extract_drawable; BitmapFactory.decodeResource/decodeStream/decodeByteArray stubs
- [x] [PARTIAL] **Canvas 2D drawing**: all draw methods stubbed (drawRect, drawCircle, drawLine, drawText, etc.); no Core Graphics bridge
- [x] [PARTIAL] **Paint / Path / Matrix**: Paint done with field-backed properties (color, strokeWidth, style, textSize, textAlign, antiAlias); Path/Matrix still missing
- [x] [DONE] **Typeface**: DEFAULT/BOLD/SERIF/MONOSPACE constants, create/defaultFromStyle
- [ ] [MISSING] **MediaPlayer**: bridge to AVAudioPlayer for audio
- [ ] [MISSING] **SoundPool**: short audio clips
- [ ] [BLOCKED] **Video playback**: would need AVPlayer bridge
- [ ] [BLOCKED] **Camera**: would need AVCaptureSession bridge
- [ ] [BLOCKED] **OpenGL ES / Vulkan**: graphics pipeline mismatch
- [ ] [RESEARCH] Simple Canvas → Core Graphics translation for custom views

---

### 3.16 Concurrency and Threading

- [x] Thread.start() runs synchronously
- [x] monitor-enter/exit are noops
- [x] [DONE] **Cooperative threading model**: Thread.start() runs Runnable.run() synchronously; ExecutorService.submit() executes and returns Future with result
- [x] [DONE] **synchronized keyword semantics**: monitor-enter/exit are no-ops (single-threaded)
- [x] [DONE] **java.util.concurrent.atomic.AtomicInteger/Boolean/Reference** — field-backed with get/set/compareAndSet
- [x] [DONE] **java.util.concurrent.locks.ReentrantLock** — lock/unlock/tryLock stubs
- [x] [DONE] **java.util.concurrent.CountDownLatch** — countDown/await stubs
- [x] [DONE] **ExecutorService / ThreadPoolExecutor / ScheduledExecutorService** — submit/execute run synchronously; Executors factory methods
- [x] [DONE] **Future / CompletableFuture** — get/join/isDone; thenApply/thenAccept/thenRun execute synchronously; completedFuture
- [x] [DONE] **Kotlin coroutines**: 22 classes - CoroutineScope, Job, Deferred, Dispatchers, GlobalScope, launch/async builders, Flow/StateFlow/SharedFlow, delay, withContext, runBlocking
- [ ] [HARDEN] Detect and report potential infinite loops in synchronized blocks

---

### 3.17 Security and Sandbox Hardening

- [ ] [HARDEN] **Parser input validation**:
  - [ ] Maximum file size checks on all parsers (APK already has 100MB)
  - [ ] Maximum nesting depth in XML parsers
  - [ ] Maximum string length in string pools
  - [ ] Maximum class count sanity check
  - [ ] Maximum method count per class
- [ ] [HARDEN] **ZIP bomb detection**: reject entries with >100:1 compression ratio
- [ ] [HARDEN] **Path traversal**: sanitize all filenames from ZIP entries
- [ ] [HARDEN] **Integer overflow**: audit all size calculations (count * sizeof)
- [ ] [HARDEN] **Buffer overflow**: bounds check on all array accesses in parsers
- [ ] [HARDEN] **Stack depth**: enforce DX_MAX_STACK_DEPTH consistently (128)
- [x] [DONE] **Heap exhaustion**: OutOfMemoryError with descriptive message on alloc_object/alloc_array failure
- [x] [DONE] **Infinite loop detection**: DX_ERR_BUDGET_EXHAUSTED with method name, PC, last 16 instructions trace
- [x] [DONE] **Crash isolation**: SIGSEGV/SIGBUS signal handlers with sigsetjmp/siglongjmp recovery; DX_ERR_SIGNAL
- [x] [DONE] **Register bounds checking**: CHECK_REG macro on move/aget/aput/iget/iput; DX_ERR_VERIFICATION on violation
- [ ] [MISSING] **Fuzzing harness**: AFL/libFuzzer targets for APK, DEX, AXML, resources.arsc parsers
- [ ] [MISSING] **Memory sanitizer**: run under ASan/MSan periodically

---

### 3.18 Performance and Optimization

#### Parsing Performance
- [ ] [OPTIMIZE] Memory-mapped APK access instead of read-all-to-memory
- [ ] [OPTIMIZE] Lazy DEX string table loading (don't decode all strings upfront)
- [ ] [OPTIMIZE] Layout XML parse caching (don't re-parse same layout)

#### Interpreter Performance
- [ ] [OPTIMIZE] Computed goto dispatch (threaded interpreter) — 20-40% faster than switch
- [ ] [OPTIMIZE] Register file pinned in local variables instead of frame->registers[i] indirection
- [ ] [OPTIMIZE] Inline caching for invoke-virtual (cache receiver class → target method)
- [ ] [OPTIMIZE] Method inlining for trivial methods (getters/setters)
- [ ] [OPTIMIZE] Opcode combination (const/4 + if-eqz → const-if pattern)

#### Memory
- [ ] [OPTIMIZE] Object pooling for DxFrame (avoid malloc/free per method call)
- [ ] [OPTIMIZE] String intern table: switch from linear scan to hash table at scale
- [ ] [OPTIMIZE] Class table: switch from linear scan to hash table if >500 classes
- [ ] [OPTIMIZE] Field access: precompute field slot indices at class load time instead of name lookup
- [ ] [OPTIMIZE] Reduce DxValue size (tagged union is 16 bytes; could be 12 with NaN-boxing)

#### UI Performance
- [ ] [OPTIMIZE] Diff-based render model updates (currently rebuilds entire tree)
- [ ] [OPTIMIZE] Throttle UI updates to 60fps max
- [ ] [OPTIMIZE] Lazy child rendering for large view hierarchies

#### Profiling Infrastructure
- [ ] [MISSING] Method-level execution time profiling
- [ ] [MISSING] Heap allocation profiling (objects/sec, bytes/sec)
- [ ] [MISSING] GC pause time measurement
- [ ] [MISSING] Opcode frequency histogram
- [ ] [MISSING] Hot method identification

---

### 3.19 Observability, Debugging, and Developer Tooling

- [x] Log callback system with level filtering
- [x] 17 error codes with string conversion
- [x] Log export to clipboard
- [x] APKInspector CLI tool

#### Remaining Work
- [ ] [MISSING] **Bytecode trace mode**: log each instruction with register state (gated by method name filter)
- [ ] [MISSING] **Class load trace**: log every class loaded with source DEX file
- [ ] [MISSING] **Method call trace**: log method entry/exit with arguments
- [x] [DONE] **UI tree inspector**: dx_ui_tree_dump() with indented tree view; accessible from Logs toolbar
- [ ] [MISSING] **Resource resolution inspector**: show resource ID → value resolution chain
- [ ] [MISSING] **Manifest inspector**: in-app view of parsed manifest fields
- [ ] [MISSING] **DEX browser**: in-app class/method/field browser
- [x] [DONE] **Heap inspector**: dx_vm_heap_stats() with capacity/live/top-10 classes; accessible from Logs toolbar
- [x] [DONE] **Stack trace on error**: dx_vm_get_last_error_detail() captures method/pc/opcode/registers/call-chain; accessible from Logs toolbar
- [ ] [MISSING] **Structured log format**: JSON logs for machine processing
- [ ] [MISSING] **Log search and filter in UI**: currently filter by level only
- [ ] [MISSING] **Crash report generation**: on fatal error, produce shareable report

---

### 3.20 Testing and Quality Assurance

#### Current: 17 Unit Tests

- [x] Context lifecycle
- [x] DEX magic validation
- [x] DEX header parsing
- [x] Log system initialization
- [x] Result string conversion
- [x] Opcode names and widths
- [x] UI node tree operations
- [x] VM framework class registration
- [x] VM string creation
- [x] VM object allocation
- [x] Field hierarchy safety
- [x] AppCompatActivity registration
- [x] Render model creation
- [x] UI tree counting

#### Remaining Work
- [ ] [MISSING] **Bytecode execution tests**: isolated interpreter tests per opcode family
  - [ ] Arithmetic opcodes: verify overflow, underflow, div-by-zero
  - [ ] Control flow: verify goto, if-*, switch branch targets
  - [ ] Method invocation: verify virtual dispatch, super calls
  - [ ] Exception handling: verify throw/catch across methods
  - [ ] Array operations: verify bounds checking
  - [ ] Field access: verify iget/iput/sget/sput with inheritance
- [ ] [MISSING] **APK loading integration tests**:
  - [ ] Load minimal test APK and verify class count
  - [ ] Load APK with multiple DEX files
  - [ ] Load APK with no DEX files (error path)
  - [ ] Load corrupt APK (error path)
- [ ] [MISSING] **Layout XML parsing tests**:
  - [ ] Parse test layout with all 17 view types
  - [ ] Verify attribute extraction (text, id, gravity, padding)
  - [ ] Verify nested layout depth handling
- [ ] [MISSING] **Resource resolution tests**:
  - [ ] Look up string by ID
  - [ ] Look up color by ID
  - [ ] Look up dimension by ID
- [ ] [MISSING] **GC correctness tests**:
  - [ ] Allocate objects until GC triggers
  - [ ] Verify reachable objects survive
  - [ ] Verify unreachable objects are freed
  - [ ] Test circular references
- [ ] [MISSING] **String interning tests**:
  - [ ] Verify same string returns same object
  - [ ] Verify intern table capacity limits
- [ ] [MISSING] **Malformed input corpus**:
  - [ ] Truncated DEX files
  - [ ] Invalid DEX magic
  - [ ] Oversized string pool indices
  - [ ] Zero-length method bodies
  - [ ] Invalid opcode sequences
- [ ] [MISSING] **Regression test suite**: each fixed bug gets a test
- [ ] [MISSING] **Compatibility test matrix**: categorized APK corpus with expected outcomes
- [ ] [MISSING] **Memory leak tests**: run APK load/unload cycles and verify no growth
- [ ] [MISSING] **Performance benchmarks**: time critical paths with reproducible inputs
- [ ] [MISSING] **Fuzzing targets**: for dx_apk_open, dx_dex_parse, dx_layout_parse, dx_resources_parse

---

### 3.21 Compatibility Expansion Roadmap

#### Tier 0: Toy Demo APKs
**Status: ACHIEVED**
- Single-activity, hardcoded layout, simple click handlers
- Calculator, Hello World, counter apps

#### Tier 1: Simple Single-Activity Apps
**Status: ACHIEVED**
- Single DEX, XML layout, TextView/Button/EditText — all working
- SharedPreferences with full Editor support (put/get/commit/apply)
- EditText with SwiftUI TextField binding
- **No remaining blockers**

#### Tier 2: Ordinary Productivity/Utility Apps
**Status: MOSTLY ACHIEVED**
- Multi-activity with intents — DONE (Intent with extras, startActivity, back-stack)
- RecyclerView with adapter pattern — DONE (real adapter dispatch)
- Fragment lifecycle — DONE (onCreateView→onViewCreated→onStart→onResume)
- File I/O for data persistence — DONE (File, FileInputStream/FileOutputStream, BufferedReader/BufferedWriter stubs)
- HTTP networking for data fetch — PARTIAL (URL/HttpURLConnection stubs; no real networking)
- **Remaining blockers**: No real networking (URLSession bridge needed)
- **Required subsystems**: Real networking bridge to URLSession

#### Tier 3: Resource-Heavy Commercial Apps
**Status: PARTIALLY ACHIEVED**
- Complex theme/style hierarchies — NOT DONE (no style/theme resolution)
- Bitmap/drawable heavy — DONE (PNG/JPEG extraction from APK, ImageView renders real images)
- Multiple languages/densities — NOT DONE (no qualifier system)
- Background services — DONE (startService→onCreate→onStartCommand; IntentService)
- **Remaining blockers**: No style resolution, no qualifier system
- **Required subsystems**: Full resources.arsc resolution, style/theme, qualifier precedence

#### Tier 4: JNI-Heavy Apps
**Status: NOT ACHIEVABLE (JAILED iOS)**
- NDK shared libraries (.so files)
- JNI call bridge for performance-critical code
- **Blockers**: Cannot execute unsigned ARM ELF binaries
- **Feasibility**: Only for apps where native code is non-essential (analytics, logging)
- **Research**: Detect JNI dependency and report; stub common native libs

#### Tier 5: Complex Apps (Services, Reflection, Threading)
**Status: PARTIALLY ACHIEVED**
- Dependency injection (Dagger/Hilt) — NOT DONE (needs Proxy.newProxyInstance)
- Reactive programming (RxJava, Coroutines with real async) — PARTIAL (Kotlin coroutines stubbed; RxJava classes registered)
- Retrofit/OkHttp networking — PARTIAL (classes registered; no real networking)
- Room database — PARTIAL (SQLiteDatabase stub with empty Cursor)
- **Achieved**: Method.invoke with real dispatch, SQLiteDatabase stub, Service lifecycle, 360+ framework classes
- **Remaining blockers**: No real threading, no real networking, no dynamic proxy
- **Required subsystems**: Cooperative threading, networking bridge, Proxy.newProxyInstance

#### Tier 6: Games / Graphics-Heavy / Engine-Based
**Status: NOT ACHIEVABLE**
- Unity, Unreal, Godot, LibGDX
- OpenGL ES / Vulkan rendering
- Real-time frame loops at 60fps
- **Blockers**: No GPU pipeline, no native code execution
- **Feasibility**: Essentially impossible on jailed iOS

#### Tier 7: Practically Impossible
- Jetpack Compose (requires Compose compiler plugin runtime)
- Flutter (requires Dart VM + Skia native engine)
- React Native (requires V8/Hermes JS engine + native bridge)
- Google Play Services dependent apps
- System apps (require Android system permissions)

---

### 3.22 Platform Constraint Ledger

| Constraint | Impact | Severity | Workaround | Feasibility |
|-----------|--------|----------|------------|-------------|
| No JIT compilation | Must interpret all bytecode; ~100-1000x slower than native | High | Optimize interpreter; consider AOT for hot methods | Partially feasible |
| No unsigned native code | Cannot run .so files from APKs | Critical | Stub JNI; detect and report | No workaround for arbitrary .so |
| iOS sandbox | Limited FS access; no /proc /sys /dev | Medium | Map paths to sandbox dirs | Feasible |
| No Binder IPC | Cannot emulate Android system services | High | Stub service interfaces | Partial stubs feasible |
| No fork/exec | Cannot spawn processes | Medium | Single-process model | Feasible (most apps are single-process) |
| No background execution | iOS suspends aggressively | Medium | Run in foreground only | Feasible with limitations |
| Graphics mismatch | No OpenGL ES context; no SurfaceFlinger | Critical for games | SwiftUI translation for views; games blocked | View-based apps feasible; games not |
| Memory pressure | iOS kills at ~1-1.5GB | Medium | GC and memory limits | Feasible with tuning |
| No Linux kernel | Missing syscalls, procfs, sysfs | Low-Medium | Stub where needed | Feasible for most app code |
| No ptrace/debugging | Cannot debug child processes | Low | Internal diagnostics only | Feasible |
| App Store restrictions | May reject apps that execute arbitrary code | Critical | Interpreter-only; no code generation | Legal/policy risk exists |

---

## 4. Implementation Status Matrix

| Subsystem | Status | MVP Required | Broad Compat | Near-Universal | iOS Feasibility | Priority | Notes |
|-----------|--------|-------------|--------------|----------------|-----------------|----------|-------|
| ZIP/APK parser | Functional | Yes | Yes | Yes | Full | - | Missing ZIP64 |
| AXML parser | Functional but limited | Yes | Yes | Yes | Full | Medium | Missing SDK version, label |
| Manifest parsing | Functional but limited | Yes | Yes | Yes | Full | Medium | Missing meta-data, features |
| Resources.arsc parsing | Functional but limited | Yes | Yes | Yes | Full | High | Missing styles, themes, qualifiers |
| DEX parsing | **Functional** | Yes | Yes | Yes | Full | Done | Annotations + debug info parsed; 14/14 types |
| DEX verification | Not started | No | Yes | Yes | Full | Medium | |
| Bytecode interpreter | **Functional** | Yes | Yes | Yes | Full | Done | All 256 opcodes covered (stubs for polymorphic/custom) |
| Exception unwinding | **Functional** | Yes | Yes | Yes | Full | Done | Cross-method unwinding via DX_ERR_EXCEPTION |
| Class loading | Functional | Yes | Yes | Yes | Full | Low | |
| VTable dispatch | Functional | Yes | Yes | Yes | Full | Low | |
| GC | Functional | Yes | Yes | Yes | Full | Low | Mark-sweep works |
| String handling | Functional | Yes | Yes | Yes | Full | Low | Interning works |
| Reflection | **Functional** | No | Yes | Yes | Full | Done | Class.forName, Method.invoke, Field.get/set with real dispatch |
| Threading | Simplified | No | Yes | Yes | Limited | Medium | Synchronous only |
| JNI bridge | **Functional** | No | Yes | Yes | Limited | Done | 232 functions; Call*Method, Get/Set*Field, RegisterNatives, exception tracking |
| .so loading | Not started | No | No | Tier 4+ | **Blocked** | - | Cannot execute on jailed iOS |
| Activity lifecycle | **Functional** | Yes | Yes | Yes | Full | Done | Full lifecycle + multi-activity + back-stack |
| Fragment lifecycle | **Functional** | No | Yes | Yes | Full | Done | onCreateView→onViewCreated→onStart→onResume |
| Service lifecycle | **Functional** | No | No | Yes | Limited | Done | startService→onCreate→onStartCommand; IntentService |
| Layout inflation | Functional | Yes | Yes | Yes | Full | Low | 30+ view types |
| ConstraintLayout | Simplified | No | Yes | Yes | Full | High | Rendered as ZStack |
| RecyclerView | **Functional** | No | Yes | Yes | Full | Done | Real adapter pattern with getItemCount/onCreateViewHolder/onBindViewHolder |
| View input (EditText) | **Functional** | Yes | Yes | Yes | Full | Done | SwiftUI TextField with text binding |
| ImageView | **Functional** | No | Yes | Yes | Full | Done | Loads PNG/JPEG from APK res/drawable-* |
| File I/O | **Functional** | No | Yes | Yes | Full | Done | File with path storage; FileInputStream/FileOutputStream/BufferedReader/BufferedWriter stubs |
| Networking | **Stub** | No | Yes | Yes | Full | Medium | URL/HttpURLConnection/HttpsURLConnection stubs; no real networking |
| SQLite | **Stub** | No | No | Yes | Full | Medium | SQLiteDatabase with empty Cursor, transaction support |
| SharedPreferences | **Functional** | Yes | Yes | Yes | Full | Done | In-memory persistence via Editor |
| Looper/Handler | **Functional** | No | Yes | Yes | Full | Done | post/postDelayed synchronous; Looper.getMainLooper/myLooper return objects |
| Canvas/Graphics | **Stub** | No | No | Yes | Partial | Low | Canvas draw methods stubbed; Paint field-backed; no Core Graphics bridge |
| Animation | **Functional** | No | No | Yes | Full | Done | ValueAnimator, ObjectAnimator, AnimatorSet, view.animation.* stubs |
| WebView | Not started | No | No | Yes | Limited | Low | Could bridge WKWebView |
| Compose | Not started | No | No | No | **Blocked** | - | Requires compiler runtime |
| Test coverage | Partial (17 tests) | Yes | Yes | Yes | Full | High | No integration tests |
| Fuzzing | Not started | No | Yes | Yes | Full | Medium | |
| CI/CD | Not started | Yes | Yes | Yes | Full | Medium | |

---

## 5. Detailed Unfinished Task Registry

### 5.1 Parser Hardening
- [x] Validate central directory entry count against actual entries parsed
- [ ] Validate local file header CRC32 against central directory CRC32
- [x] Reject ZIP entries with filename containing ".." path traversal
- [x] Add maximum decompressed size check (reject if >500MB decompressed)
- [x] Add maximum string pool size check in AXML parser (reject if >1M strings)
- [x] Add maximum XML nesting depth (reject if >100 levels)
- [x] Validate DEX header endian_tag field (0x12345678 expected)
- [ ] Validate DEX map_off points to valid map_list structure
- [x] Validate all DEX table offsets are within file bounds before accessing
- [ ] Add fuzz-friendly entry points: dx_apk_open_fuzz(data, size), dx_dex_parse_fuzz(data, size)

### 5.2 DEX Parsing Completions
- [x] Parse annotation_set_item at class_def.annotations_off
- [x] Parse annotation_item structures (visibility, type_idx, elements)
- [x] Parse encoded_annotation (type_idx, element count, name-value pairs)
- [x] Store parsed annotations on DxClass and DxMethod structs
- [x] Parse debug_info_item header (line_start, parameters_size, parameter_names)
- [x] Parse debug_info state machine bytecodes (DBG_ADVANCE_PC, DBG_ADVANCE_LINE, etc.)
- [x] Build line number table from debug info (binary search lookup via dx_method_get_line)
- [x] Parse encoded_value VALUE_ARRAY (skip contents, advance pointer correctly)
- [x] Parse encoded_value VALUE_ANNOTATION (skip contents, advance pointer correctly)
- [ ] Validate code_item.tries_size matches actual try_item count
- [ ] Validate code_item.insns_size matches actual instruction count

### 5.3 Interpreter Correctness
- [x] Implement cross-method exception unwinding (throw in callee → catch in caller)
- [x] Generate NullPointerException on null dereference in iget/iput
- [x] Generate ArrayIndexOutOfBoundsException on array access (aget out of bounds)
- [x] Generate ClassCastException on failed check-cast
- [x] Handle INT_MIN / -1 division (INT_MIN for div, 0 for rem — all int and long variants)
- [x] Support fill-array-data with 8-byte elements (long/double)
- [x] Validate switch payload identifiers (0x0100 for packed, 0x0200 for sparse) — already done
- [x] Bounds check switch payload offset against code_size (added bounds check before payload access)
- [ ] Verify goto target is a valid instruction boundary (not middle of wide instruction)

### 5.4 VM Core
- [x] Implement call-stack unwinding for exceptions (DX_ERR_EXCEPTION propagation)
- [x] Make Handler.post() execute Runnable synchronously
- [x] Make Handler.postDelayed() execute Runnable synchronously
- [x] Make System.currentTimeMillis() return actual wall clock time
- [x] Make Log.d/i/w/e forward to DexLoom DX_DEBUG/DX_INFO/DX_WARN/DX_ERROR
- [x] Implement Resources.getString(int) via resources.arsc lookup
- [x] Implement Context.getPackageName() returning manifest package name
- [x] Implement Context.getFilesDir() returning mapped sandbox path
- [x] Object.hashCode() already returns stable identity hash (pointer-based)
- [x] Object.getClass() already returns runtime class descriptor as string

### 5.5 Framework Class Implementations
- [x] Implement SharedPreferences with in-memory key-value store (256 entries)
- [x] Implement SharedPreferences.Editor with putString/putInt/putBoolean/putLong/putFloat/commit/apply
- [x] Implement AlertDialog.Builder with chaining + create/show (simulated)
- [x] Implement Toast.makeText with logging + object return
- [x] Implement File class with path storage + simulated filesystem ops
- [x] Implement FileInputStream / FileOutputStream as stubs (registered with inheritance)
- [x] Implement BufferedReader / BufferedWriter / PrintWriter as stubs
- [x] Implement java.net.URL with parsing (constructor stores URL, toString/openConnection)
- [x] Implement HttpURLConnection with stub responses (URLSession bridge deferred)
- [x] Implement android.util.SparseArray / SparseIntArray / LongSparseArray with stub storage
- [x] Implement android.text.SpannableString + SpannableStringBuilder (basic; no real spans)
- [x] Implement android.os.Parcelable.Creator interface stub
- [x] Implement Kotlin.Result wrapper class (isSuccess/isFailure/getOrNull)
- [x] Implement kotlin.collections.MapsKt, ArraysKt, kotlin.text.StringsKt utility functions

### 5.6 UI Improvements
- [x] Implement EditText as SwiftUI TextField with binding
- [x] Implement onTextChanged callback for EditText (via dx_runtime_update_edit_text + TextWatcher)
- [x] Implement ImageView with APK drawable loading (dx_ui_extract_drawable, PNG/JPEG via UIImage)
- [x] Implement RecyclerView with adapter.getItemCount/onBindViewHolder pattern
- [x] Implement Spinner with adapter/selection stubs (SwiftUI Picker deferred)
- [x] Implement AlertDialog.Builder create/show (backend done; SwiftUI presentation deferred)
- [x] Implement individual padding (paddingLeft/Top/Right/Bottom) with dp conversion
- [x] Implement RelativeLayout basic positioning (VStack instead of ZStack)
- [ ] Implement ConstraintLayout basic constraint solver (left/right/top/bottom anchoring)
- [x] Implement dp-to-points conversion (1:1 mapping with helper functions)
- [ ] Add touch event propagation beyond button clicks
- [ ] Implement long-press gesture recognition
- [ ] Implement swipe/scroll gesture in ScrollView

### 5.7 JNI Bridge
- [x] Implement CallIntMethod dispatching to dx_vm_execute_method
- [x] Implement CallBooleanMethod dispatching to dx_vm_execute_method
- [x] Implement CallLongMethod dispatching to dx_vm_execute_method
- [x] Implement GetIntField using dx_vm_get_field
- [x] Implement SetIntField using dx_vm_set_field
- [x] Implement GetStaticObjectField from DxClass static_fields
- [x] Implement NewObject calling <init> after allocation
- [x] Track pending exception state (ThrowNew sets flag, ExceptionCheck returns it, ExceptionClear resets it)
- [x] Implement RegisterNatives binding function pointers to DxMethod.native_fn

### 5.8 Testing
- [ ] Create test APK fixture with minimal valid DEX bytecode
- [ ] Test: load test APK, verify class count and method count
- [ ] Test: execute simple bytecode (const/4 → return)
- [ ] Test: verify arithmetic opcodes (add, sub, mul, div overflow)
- [ ] Test: verify if-eq/ne/lt/ge branch correctness
- [ ] Test: verify invoke-virtual dispatches to subclass override
- [ ] Test: verify invoke-super calls parent implementation
- [ ] Test: verify try/catch within single method
- [ ] Test: verify try/catch across method boundary (after unwinding implemented)
- [ ] Test: verify GC frees unreachable objects
- [ ] Test: verify GC preserves reachable objects
- [ ] Test: verify string interning deduplication
- [ ] Test: load malformed DEX (truncated) without crash
- [ ] Test: load malformed APK (truncated) without crash
- [ ] Test: instruction budget exhaustion returns gracefully
- [ ] Test: stack depth limit triggers DX_ERR_STACK_OVERFLOW

---

## 6. Simplified or Placeholder Implementation Debt

### ~~Fake Parse Success~~ [FIXED]
- ~~**Manifest SDK version extraction**: Fields exist in DxManifest but never populated.~~ [FIXED] minSdkVersion and targetSdkVersion now extracted from uses-sdk tag.
- ~~**App label extraction**: Field exists but returns NULL.~~ [FIXED] android:label now extracted from application tag.

### ~~Hardcoded Values~~ [FIXED]
- ~~**System.currentTimeMillis() returns 0**~~: [FIXED] Now returns actual wall clock time.
- ~~**SharedPreferences returns default values**~~: [FIXED] In-memory key-value store with Editor put/get/commit/apply (256 entries).

### ~~Demo-Only Resource Substitution~~ [FIXED]
- ~~**ImageView placeholder**~~: [FIXED] Now loads actual PNG/JPEG from APK res/drawable-* via dx_ui_extract_drawable.
- ~~**RecyclerView as VStack**~~: [FIXED] Real adapter pattern with getItemCount/onCreateViewHolder/onBindViewHolder dispatch.

### Non-Validating Readers
- **DEX bytecode trusted without verification**: Malformed bytecode could read out of bounds on code[], corrupt register state, or infinite loop. **Dangerous** for untrusted APKs. **Fix**: Add verification pass.
- **ZIP entries trusted without CRC validation**: Corrupt data passes silently. **Fix**: Validate CRC32.

### Logging-Only Execution Paths
- ~~**Handler.post() is a no-op**~~: [FIXED] Now executes Runnables synchronously.
- ~~**ViewGroup.addView() is a no-op**~~: [FIXED] Now creates DxUINode and attaches to parent tree, triggers render model rebuild.

### Unsupported Features Silently Ignored
- **monitor-enter/exit no-ops**: Synchronized blocks compile to these opcodes. Code inside runs regardless, which is correct for single-threaded, but apps relying on synchronization for correctness may exhibit wrong behavior.
- **Networking stubs return simulated 200 responses**: HttpURLConnection returns stub InputStream; no real data. Apps that depend on network data will show empty/default content. **Fix**: Bridge to URLSession for real networking.

---

## 7. Optimization and Scaling Backlog

### Parser Performance
- [ ] Memory-map APK file instead of reading entire file into memory
- [ ] Lazy DEX string table: only decode strings when accessed
- [ ] Cache parsed layout XML by resource ID (avoid re-parsing)
- [ ] Cache parsed class data by class_def index

### Interpreter Performance
- [ ] Computed goto dispatch table (threaded interpreter)
- [ ] Pin hot registers in local C variables within interpreter loop
- [ ] Inline caching for monomorphic call sites (invoke-virtual)
- [ ] Superinstruction combining (e.g., iget + return-object → single dispatch)
- [ ] Method inlining for trivial getters/setters (field access + return)

### Memory Efficiency
- [x] Frame pool: 64-frame pool eliminates malloc/free per method call
- [ ] String intern hash table (replace linear scan for >1000 strings)
- [x] Class lookup hash table: FNV-1a O(1) lookup (DX_CLASS_HASH_SIZE=4096)
- [ ] Field slot precomputation (store slot index at class load, avoid name lookup at runtime)
- [ ] NaN-boxing for DxValue (reduce from 16 bytes to 8 bytes per register)
- [ ] Arena allocator for parse-time allocations (DEX strings, type IDs)

### UI Performance
- [ ] Diff-based render model: only serialize changed nodes
- [ ] Throttle render model updates to 16ms intervals
- [ ] Lazy child expansion for large view hierarchies (>100 nodes)
- [ ] View recycling for RecyclerView

### Benchmarking
- [ ] Create benchmark APK with known instruction count
- [ ] Measure opcodes-per-second on simulator
- [ ] Measure GC pause duration
- [ ] Measure APK parse time for 10MB, 50MB, 100MB APKs
- [ ] Measure layout inflation time for 10, 50, 200 node layouts

---

## 8. Production Hardening Backlog

- [ ] **Crash isolation**: Install signal handlers for SIGSEGV/SIGBUS; report error instead of crashing app
- [ ] **Memory pressure handling**: Register for iOS memory warnings; trigger aggressive GC
- [ ] **Watchdog**: Detect interpreter stuck for >10 seconds; abort with diagnostic
- [ ] **UI freeze prevention**: Run interpreter on background thread; bridge results to main thread
- [ ] **Deterministic diagnostics**: On any DxResult error, capture: method name, pc, opcode, register snapshot
- [ ] **Large APK resilience**: Stream-process APKs >50MB instead of loading to memory
- [ ] **Corruption resistance**: Validate all pointer dereferences in interpreter hot path
- [ ] **Graceful unsupported features**: When an APK uses unsupported features, present user-facing message listing what's missing
- [ ] **Release gating criteria**: Define minimum test pass rate (e.g., 95% of test suite) before release
- [ ] **Telemetry**: Optional opt-in crash/compatibility reporting

---

## 9. Research Backlog

- [ ] **[RESEARCH] JNI on jailed iOS**: Can ARM user-mode emulation (Unicorn) execute simple .so JNI methods with acceptable performance? Benchmark.
- [ ] **[RESEARCH] Partial Linux API emulation**: How many Android-used syscalls can be stubbed on iOS? Which ones cause real failures?
- [ ] **[RESEARCH] Jetpack Compose feasibility**: Could a Compose compiler output translator generate SwiftUI? (Likely no, but worth studying the IR.)
- [ ] **[RESEARCH] WebView via WKWebView**: Can android.webkit.WebView be bridged to WKWebView for basic page loads? JavaScript bridge?
- [ ] **[RESEARCH] Canvas → Core Graphics**: Map Android Canvas draw calls to CGContext. What subset is feasible?
- [ ] **[RESEARCH] invoke-custom / LambdaMetafactory**: Can lambda desugaring be done at DEX load time instead of runtime?
- [ ] **[RESEARCH] Split APK support**: Parse split_config.*.apk and merge resources. How complex is the merge?
- [ ] **[RESEARCH] AOT compilation**: Could a DEX-to-C translator generate compilable C source for hot methods?
- [ ] **[RESEARCH] Dynamic proxy**: Can Proxy.newProxyInstance be implemented by generating DxClass/DxMethod at runtime?
- [ ] **[RESEARCH] App compatibility database**: Crowdsourced database of APK → compatibility status for user guidance

---

## 10. Priority Roadmap

### Phase 1: Critical Path to Serious Basic APK Support
1. **Fix exception unwinding across method boundaries** — correctness blocker
2. **Implement Handler.post/postDelayed synchronous execution** — many apps need this
3. **Implement Resources.getString via resources.arsc** — apps show blank text without it
4. **Implement basic File I/O** (File, FileInputStream, FileOutputStream) — persistence
5. **Implement SharedPreferences with real persistence** — state management
6. **Implement EditText with keyboard input** — user interaction
7. **Complete manifest extraction** (SDK version, app label) — metadata
8. **Add integration tests** for APK loading and bytecode execution

### Phase 2: Critical Path to Broad APK Support
1. **Implement java.net.HttpURLConnection** bridged to URLSession — networking
2. **Implement RecyclerView adapter pattern** — most list-based apps
3. **Implement style/theme resolution** from resources.arsc — visual correctness
4. **Implement resource qualifier system** (density, locale, API level) — correct assets
5. **Implement reflection** (Method.invoke, Field.get/set) — Dagger/Retrofit/many libs
6. **Implement Looper/MessageQueue** — deferred initialization
7. **Implement drawable loading** (PNG/JPEG from APK) — images
8. **Implement AlertDialog/Toast** — user feedback
9. **Add fuzzing infrastructure** for all parsers
10. **Implement ViewGroup.addView** for programmatic UI construction

### Phase 3: Critical Path to Advanced Compatibility
1. **Implement SQLite** bridged to iOS SQLite — database apps
2. **Implement cooperative threading** — concurrency-dependent apps
3. **Implement ConstraintLayout constraint solver** (basic subset)
4. ~~**Implement multi-activity navigation** with Intent dispatch~~ DONE
5. **Implement Canvas → Core Graphics** bridge for custom views
6. ~~**Implement annotation parsing**~~ DONE — type+visibility stored on DxClass/DxMethod
7. **Complete JNI primitive method dispatch**
8. **Implement WebView → WKWebView** bridge
9. ~~**Implement full Activity lifecycle** sequence~~ DONE (onCreate→onPostCreate→onStart→onResume→onPostResume + teardown on navigation)
10. **Performance optimization pass** (computed goto, ~~frame pooling~~ DONE, string hash)

### Phase 4: Long-Term Research
1. ARM user-mode emulation for select .so files
2. Compose IR translation study
3. AOT compilation research
4. Dynamic proxy generation
5. App compatibility database

### Phase 5: Permanent Platform Limitations
These items cannot be solved and should be clearly documented for users:
- Arbitrary .so execution
- OpenGL ES / Vulkan rendering
- Google Play Services
- Jetpack Compose
- Game engines (Unity, Unreal)
- Camera/Bluetooth/NFC hardware APIs
- Background services matching Android behavior

---

## 11. Definition of Quality Bars

### Parser Quality
- Rejects all malformed inputs without crashing
- Returns specific error codes for each failure type
- No unbounded memory allocation from untrusted input
- Fuzz-tested for 1M+ iterations without crashes

### Runtime Quality
- All implemented opcodes match Android semantics for normal inputs
- Exception handling propagates correctly across method boundaries
- GC does not free reachable objects
- GC does not leak unreachable objects
- Instruction budget prevents infinite execution

### Compatibility Quality
- Tier 1 APKs (simple single-activity) produce correct visual output 90%+ of the time
- Tier 2 APKs (productivity/utility) produce recognizable output 50%+ of the time
- Unsupported features produce clear diagnostic messages, not silent failures

### UI Fidelity
- Text renders with correct content, size, and color
- Layout structure (linear, frame) matches Android for simple cases
- Buttons respond to taps
- Scroll views scroll
- Padding and margin are visually correct

### Performance
- APK parse time: <2 seconds for 50MB APK
- Layout inflation: <500ms for 100-node layout
- Interpreter: >1M opcodes/second on modern iOS device
- GC pause: <50ms
- UI update: <16ms (60fps target)

### Observability
- Every error path produces a log entry
- Fatal errors include: method name, program counter, opcode
- UI provides log filtering and export

### Security
- No path traversal from ZIP entries
- No buffer overflow from malformed DEX
- No stack overflow from deep recursion (frame heap allocation)
- No infinite loop without budget exhaustion

### Documentation
- All public C APIs have header comments
- Architecture document reflects current state
- Known limitations are documented

### Test Coverage
- All parser entry points have at least one positive and one negative test
- All opcode families have execution tests
- GC has correctness tests
- Integration test with real APK exists

---

## 12. Done Criteria

A task may be marked done when ALL of the following are true:

1. **Real functionality exists**: The feature performs its documented purpose, not just a placeholder or log message.

2. **No placeholder logic**: No hardcoded return values, no "TODO" in the path, no demo-only behavior.

3. **Malformed input handling**: If the feature processes external input (APK data, DEX bytecode, user interaction), it handles malformed input without crashing and produces a useful error.

4. **Tests exist**: At least one unit or integration test validates the feature's correctness. For parsers: one positive and one negative test. For interpreter opcodes: one execution test.

5. **Diagnostics exist**: Failures produce log messages with enough information to diagnose the issue (method name, offset, expected vs actual).

6. **Performance is acceptable**: The feature does not introduce visible lag (>100ms) for typical inputs. Parse operations complete in reasonable time for realistic file sizes.

7. **Unsupported behavior is explicit**: If the feature cannot handle certain inputs (e.g., an unsupported resource type), it logs a clear warning rather than silently returning wrong results.

8. **Memory is managed**: All allocations have corresponding frees. No leaks on the normal path. GC integration is correct (objects are roots if needed).

9. **Code review passed**: Another engineer (or AI agent) has read the implementation and confirmed it matches the specification.

10. **Documentation updated**: If the feature changes public API surface, header comments and relevant docs are updated.

---

*This document was generated from a comprehensive audit of the DexLoom repository on 2026-03-09. It should be updated as tasks are completed.*
