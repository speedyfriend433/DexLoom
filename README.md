# DexLoom

**Run Android APKs on iOS.** DexLoom is a native iOS app that loads Android APK files and executes their Dalvik bytecode through a custom interpreter, rendering Android UI via SwiftUI.

Built entirely in C (runtime core) and Swift (UI shell), DexLoom runs on jailed iOS devices with no jailbreak required.

## What It Does

DexLoom parses APK containers, interprets all 256 Dalvik opcodes, emulates 450+ Android/Java framework classes, and translates Android XML layouts into live SwiftUI views — complete with Activity lifecycle, touch events, and real networking.

**This is not an Android emulator.** It's a from-scratch Dalvik bytecode interpreter with a compatibility layer that bridges the gap between Android APIs and iOS capabilities.

## Features

### Interpreter & VM
- **All 256 Dalvik opcodes** including invoke-custom, invoke-polymorphic, const-method-handle/type
- **Computed goto dispatch** with 256-entry label table (20-40% faster than switch)
- **Bytecode verifier** — two-pass structural verification before execution
- **Cross-method exception unwinding** with try/catch/finally support
- **Generational GC** — young/old generations, minor/major collection cycles
- **Inline caching** for polymorphic call sites
- **Method inlining** for trivial getters/setters
- **Superinstructions** — fused opcode pairs (const/4+if-eqz, iget+return-object)
- **Frame pooling** — 64-frame pool eliminates per-call malloc/free
- **Class hash table** — FNV-1a O(1) lookup (4096-entry open addressing)
- **Arena allocator** for DEX parse-time allocations
- **NaN-boxing** macros for optional 8-byte register encoding

### Framework Classes (450+)
- **Android core**: Activity, Fragment, Service, BroadcastReceiver, ContentProvider, Context, Intent, Bundle
- **Views (30+ types)**: TextView, Button, EditText, ImageView, RecyclerView, ListView, WebView, FAB, TabLayout, ViewPager, and more
- **Jetpack**: LiveData, ViewModel, ViewModelProvider, Fragment lifecycle, Room (17 annotations + RoomDatabase)
- **Third-party**: RxJava3, OkHttp3 (real networking), Retrofit2, Glide
- **Java stdlib**: String (35+ methods), HashMap, ArrayList, Collections, Arrays, ByteBuffer, WeakReference
- **System services**: ClipboardManager, ConnectivityManager, PowerManager, NotificationManager, LocationManager
- **Database**: SQLiteDatabase (insert/update/delete/rawQuery), ContentValues, Cursor

### Real Networking
- HTTP GET/POST/PUT/DELETE via URLSession bridge
- OkHttp3 Request.Builder with real network calls
- HttpURLConnection with request/response headers and status codes
- TCP sockets via POSIX networking

### UI Rendering
- **30+ Android view types** rendered as SwiftUI components
- **ConstraintLayout** solver with 12 constraint attributes and GeometryReader positioning
- **RecyclerView** with real adapter pattern and LazyVStack
- **Vector drawables** — AXML vector XML parsed and rendered via SwiftUI Canvas
- **Property animations** — alpha, rotation, scale, translation
- **Touch events** — onClick, long-press, SwipeRefreshLayout
- **WebView** mapped to WKWebView for real web content
- **60fps throttle** with diff-based render model updates

### Activity & Navigation
- Full lifecycle: onCreate → onStart → onResume → onPause → onStop → onDestroy
- State save/restore via onSaveInstanceState/onRestoreInstanceState
- Back-stack navigation (16-deep) with startActivityForResult/setResult/finish

### Parser Hardening
- ZIP bomb detection, path traversal protection, CRC32 validation
- Encrypted entry detection, ZIP64 support
- APK signature block parsing (V2/V3 scheme detection)
- AXML nesting depth limits, string pool caps, integer overflow protection
- DEX checksum (Adler32) and SHA-1 validation
- Memory-mapped file access with streaming extraction
- App Bundle (.aab) support with `base/` path fallback
- Crash isolation via SIGSEGV/SIGBUS signal handlers with sigsetjmp/siglongjmp

### Developer Tools
- **DEX Browser** — searchable class list with method/field detail
- **Manifest Inspector** — package info, activities, services, permissions
- **Resource Inspector** — hex ID lookup, filterable resource list
- **Debug tracing** — bytecode trace, method entry/exit, class load trace
- **Profiling** — method timing, opcode histogram, GC pause measurement
- **Telemetry** — instruction count, GC stats, method invocations, exceptions

## Architecture

```
┌─────────────────────────────────────┐
│           SwiftUI Shell             │
│  Home │ Runtime │ Inspector │ Logs  │
├─────────────────────────────────────┤
│         RuntimeBridge.swift         │
│    C ↔ Swift bridging layer         │
├─────────────────────────────────────┤
│            C Runtime Core           │
│  ┌─────┐ ┌─────┐ ┌──────────────┐  │
│  │ APK │ │ DEX │ │  Interpreter │  │
│  │Parser│ │Parser│ │  (computed  │  │
│  │     │ │     │ │   goto)     │  │
│  └──┬──┘ └──┬──┘ └──────┬───────┘  │
│     │       │            │          │
│  ┌──┴───────┴────────────┴───────┐  │
│  │           VM Core             │  │
│  │  GC │ ClassLoader │ JNI      │  │
│  └───────────────────────────────┘  │
│  ┌───────────────────────────────┐  │
│  │    Android Framework (450+)   │  │
│  │  Activity │ Views │ Services  │  │
│  └───────────────────────────────┘  │
│  ┌───────────────────────────────┐  │
│  │         UI Bridge             │  │
│  │  Layout XML → Render Model    │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

| Layer | Language | Lines | Purpose |
|-------|----------|-------|---------|
| Core Runtime | C | ~45K | Parsers, interpreter, VM, GC, framework classes |
| UI Shell | Swift | ~8K | SwiftUI views, bridge, app lifecycle |
| Tests | Swift | ~6K | 242 tests across 53 suites |

## Building

**Requirements:**
- Xcode 26.0+
- iOS 26.2+ deployment target
- macOS with Apple Silicon or Intel

```bash
# Clone
git clone https://github.com/speedyfriend433/DexLoom.git
cd DexLoom

# Build
xcodebuild -scheme DexLoom \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  build

# Run tests
xcodebuild -scheme DexLoom \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  test
```

Or open `DexLoom.xcodeproj` in Xcode and hit Run.

## Usage

1. **Import an APK** — tap the import button on the Home screen and select an `.apk` or `.aab` file
2. **Inspect** — browse DEX classes, manifest details, and resources in the Inspector tab
3. **Run** — tap "Run Activity" to execute the main Activity's lifecycle
4. **View logs** — check the Logs tab for runtime output, warnings, and errors

## Test Suite

242 tests across 53 suites covering:

- **Bytecode execution** — arithmetic, branching, invoke dispatch, exception handling
- **Framework classes** — String, ArrayList, HashMap, Activity, Intent, networking stubs
- **Parser hardening** — malformed DEX, truncated APK, invalid AXML, corrupt ZIP
- **GC correctness** — reachability, circular references, weak references, generational collection
- **Memory safety** — leak detection, rapid create/destroy cycles, sanitizer integration
- **Performance benchmarks** — ops/sec, GC pause, parse throughput, layout inflation
- **Compatibility matrix** — framework class coverage, essential class registration
- **Regression tests** — guards against known past issues

## Known Limitations

- **Jetpack Compose**: Fundamentally unsupported (requires Compose compiler runtime)
- **JNI native code**: Cannot load `.so` files (no dlopen on jailed iOS)
- **OpenGL ES / Vulkan**: No graphics pipeline translation
- **Google Play Services**: Proprietary, binary-only — cannot be reimplemented
- **Full threading**: Interpreter is single-threaded; Thread.start() runs synchronously
- **Camera / Bluetooth / NFC / Telephony**: Require platform-specific bridges not yet implemented

## Documentation

Detailed docs are in the [`Docs/`](Docs/) directory:

- [Architecture](Docs/Architecture.md) — module hierarchy, data flow, key structs
- [DEX Support Matrix](Docs/DEXSupportMatrix.md) — all 256 opcodes with implementation status
- [JNI API](Docs/JNI-API.md) — 229 JNI function slots documented
- [Android Mini API](Docs/AndroidMiniAPI.md) — framework class reference
- [Runtime Design](Docs/RuntimeDesign.md) — interpreter internals
- [Troubleshooting](Docs/Troubleshooting.md) — 9 failure patterns, debug tracing guide
- [Roadmap](Docs/Roadmap.md) — project status and future directions

## Project Stats

- **59K+ lines** of source code (C + Swift)
- **450+** Android/Java framework classes
- **256/256** Dalvik opcodes implemented
- **242** automated tests
- **0** third-party dependencies

## License

MIT License. All rights reserved.
