import Foundation
import Combine

struct LogEntry: Identifiable {
    let id = UUID()
    let timestamp: Date
    let level: String
    let tag: String
    let message: String
}

/// ConstraintLayout constraint anchors for a child view
struct ConstraintAnchors {
    let leftToLeft: UInt32      // 0 = none, 0xFFFFFFFF = parent, else view ID
    let leftToRight: UInt32
    let rightToRight: UInt32
    let rightToLeft: UInt32
    let topToTop: UInt32
    let topToBottom: UInt32
    let bottomToBottom: UInt32
    let bottomToTop: UInt32
    let horizontalBias: Float   // 0.0-1.0, default 0.5
    let verticalBias: Float

    var hasHorizontal: Bool {
        let hasLeft = leftToLeft != 0 || leftToRight != 0
        let hasRight = rightToRight != 0 || rightToLeft != 0
        return hasLeft || hasRight
    }
    var hasVertical: Bool {
        let hasTop = topToTop != 0 || topToBottom != 0
        let hasBottom = bottomToBottom != 0 || bottomToTop != 0
        return hasTop || hasBottom
    }
    /// True if constrained on both left and right (centered or stretched)
    var isCenteredH: Bool {
        let hasLeft = leftToLeft != 0 || leftToRight != 0
        let hasRight = rightToRight != 0 || rightToLeft != 0
        return hasLeft && hasRight
    }
    /// True if constrained on both top and bottom
    var isCenteredV: Bool {
        let hasTop = topToTop != 0 || topToBottom != 0
        let hasBottom = bottomToBottom != 0 || bottomToTop != 0
        return hasTop && hasBottom
    }
    /// True if only left/start constrained (no right)
    var isLeftOnly: Bool {
        let hasLeft = leftToLeft != 0 || leftToRight != 0
        let hasRight = rightToRight != 0 || rightToLeft != 0
        return hasLeft && !hasRight
    }
    /// True if only right/end constrained (no left)
    var isRightOnly: Bool {
        let hasLeft = leftToLeft != 0 || leftToRight != 0
        let hasRight = rightToRight != 0 || rightToLeft != 0
        return !hasLeft && hasRight
    }
    /// True if only top constrained (no bottom)
    var isTopOnly: Bool {
        let hasTop = topToTop != 0 || topToBottom != 0
        let hasBottom = bottomToBottom != 0 || bottomToTop != 0
        return hasTop && !hasBottom
    }
    /// True if only bottom constrained (no top)
    var isBottomOnly: Bool {
        let hasTop = topToTop != 0 || topToBottom != 0
        let hasBottom = bottomToBottom != 0 || bottomToTop != 0
        return !hasTop && hasBottom
    }
    var hasAny: Bool { hasHorizontal || hasVertical }
}

struct RenderNode: Identifiable {
    let id = UUID()
    let type: DxViewType
    let viewId: UInt32
    let text: String?
    let hint: String?
    let orientation: DxOrientation
    let textSize: Float
    let width: Int32       // -1 = match_parent, -2 = wrap_content
    let height: Int32      // -1 = match_parent, -2 = wrap_content
    let weight: Float      // layout_weight (0 = none)
    let gravity: Int32
    let padding: (Int32, Int32, Int32, Int32)
    let margin: (Int32, Int32, Int32, Int32)
    let bgColor: UInt32
    let textColor: UInt32
    let isChecked: Bool
    let hasClickListener: Bool
    let hasLongClickListener: Bool
    let hasRefreshListener: Bool
    let relativeFlags: UInt16     // RelativeLayout positioning bit flags
    let relAbove: UInt32          // layout_above view ID
    let relBelow: UInt32          // layout_below view ID
    let relLeftOf: UInt32         // layout_toLeftOf view ID
    let relRightOf: UInt32        // layout_toRightOf view ID
    let constraints: ConstraintAnchors  // ConstraintLayout constraints
    let imageData: Data?          // PNG/JPEG bytes for ImageView
    let webURL: String?           // URL for WebView
    let webHTML: String?          // HTML content for WebView
    let children: [RenderNode]
}

@MainActor
final class RuntimeBridge: ObservableObject {
    @Published var logs: [LogEntry] = []
    @Published var renderTree: RenderNode?
    @Published var isLoaded = false
    @Published var isRunning = false
    @Published var errorMessage: String?
    @Published var packageName: String?
    @Published var activityName: String?
    @Published var apkEntries: [String] = []
    @Published var dexClassCount: UInt32 = 0
    @Published var dexMethodCount: UInt32 = 0
    @Published var dexStringCount: UInt32 = 0

    private var context: UnsafeMutablePointer<DxContext>?
    // Throttle log delivery to avoid flooding the main thread
    private var pendingLogs: [(String, String, String)] = []
    private var logFlushScheduled = false
    private static let maxLogs = 1000

    init() {
        addLog(level: "INFO", tag: "Bridge", message: "DexLoom runtime bridge initialized")
    }

    deinit {
        if let ctx = context {
            dx_runtime_shutdown(ctx)
        }
    }

    func loadAPK(at url: URL) {
        let path = url.path

        addLog(level: "INFO", tag: "Bridge", message: "Loading APK: \(url.lastPathComponent)")

        // Create context
        guard let ctx = dx_context_create() else {
            errorMessage = "Failed to create runtime context"
            addLog(level: "ERROR", tag: "Bridge", message: "dx_context_create failed")
            return
        }
        self.context = ctx

        // Set log level to INFO to suppress TRACE/DEBUG flooding
        dx_log_set_level(DX_LOG_INFO)

        // Set up log callback - batch logs to prevent MainActor queue flooding
        let bridge = Unmanaged.passUnretained(self).toOpaque()
        dx_log_set_callback({ level, tag, message, userData in
            guard let userData = userData else { return }
            let bridge = Unmanaged<RuntimeBridge>.fromOpaque(userData).takeUnretainedValue()
            let levelStr: String
            switch level {
            case DX_LOG_TRACE: levelStr = "TRACE"
            case DX_LOG_DEBUG: levelStr = "DEBUG"
            case DX_LOG_INFO:  levelStr = "INFO"
            case DX_LOG_WARN:  levelStr = "WARN"
            case DX_LOG_ERROR: levelStr = "ERROR"
            default:           levelStr = "?"
            }
            let tagStr = tag.map { String(cString: $0) } ?? "?"
            let msgStr = message.map { String(cString: $0) } ?? ""

            // Batch log delivery to avoid creating thousands of Task closures
            Task { @MainActor in
                bridge.enqueuLog(level: levelStr, tag: tagStr, message: msgStr)
            }
        }, bridge)

        // Set up UI update callback
        ctx.pointee.on_ui_update = { model, userData in
            guard let userData = userData, let model = model else { return }
            let bridge = Unmanaged<RuntimeBridge>.fromOpaque(userData).takeUnretainedValue()
            let tree = RuntimeBridge.convertRenderModel(model.pointee.root)
            Task { @MainActor in
                bridge.renderTree = tree
            }
        }
        ctx.pointee.ui_callback_data = bridge

        // Load APK on background thread
        Task.detached {
            let result = dx_context_load_apk(ctx, path)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                if result == DX_OK {
                    self.isLoaded = true
                    self.packageName = ctx.pointee.package_name.map { String(cString: $0) }
                    self.activityName = ctx.pointee.main_activity_class.map { String(cString: $0) }
                    if let dex = ctx.pointee.dex {
                        self.dexClassCount = dex.pointee.class_count
                        self.dexMethodCount = dex.pointee.method_count
                        self.dexStringCount = dex.pointee.string_count
                    }
                    self.addLog(level: "INFO", tag: "Bridge", message: "APK loaded successfully")
                } else {
                    let errStr = String(cString: dx_result_string(result))
                    self.errorMessage = "Load failed: \(errStr)"
                    self.addLog(level: "ERROR", tag: "Bridge", message: "Load failed: \(errStr)")
                }
            }
        }
    }

    func run() {
        guard let ctx = context, isLoaded, !isRunning else {
            if isRunning { return }  // prevent re-entry
            errorMessage = "No APK loaded"
            return
        }

        isRunning = true  // set immediately to prevent multiple taps
        addLog(level: "INFO", tag: "Bridge", message: "Starting runtime execution")

        Task.detached {
            let result = dx_context_run(ctx)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                if result == DX_OK {
                    self.addLog(level: "INFO", tag: "Bridge", message: "Runtime started successfully")
                    // Build initial render tree
                    if let model = dx_runtime_get_render_model(ctx) {
                        self.addLog(level: "INFO", tag: "Bridge", message: "Render model found, building tree")
                        self.renderTree = RuntimeBridge.convertRenderModel(model.pointee.root)
                        if self.renderTree != nil {
                            self.addLog(level: "INFO", tag: "Bridge", message: "Render tree built: type=\(self.renderTree!.type.rawValue), children=\(self.renderTree!.children.count)")
                        }
                    } else {
                        self.addLog(level: "WARN", tag: "Bridge", message: "No render model available (setContentView may not have been called)")
                    }
                } else {
                    self.isRunning = false
                    let errStr = String(cString: dx_result_string(result))
                    self.errorMessage = "Run failed: \(errStr)"
                    self.addLog(level: "ERROR", tag: "Bridge", message: "Run failed: \(errStr)")
                    if let vm = ctx.pointee.vm {
                        let vmErr = withUnsafePointer(to: &vm.pointee.error_msg) { ptr -> String in
                            ptr.withMemoryRebound(to: CChar.self, capacity: 256) { cptr in
                                // Ensure null termination by checking within bounds
                                var len = 0
                                while len < 255 && cptr[len] != 0 { len += 1 }
                                if len == 0 { return "" }
                                return String(bytes: UnsafeBufferPointer(start: UnsafePointer<UInt8>(OpaquePointer(cptr)), count: len), encoding: .utf8) ?? ""
                            }
                        }
                        if !vmErr.isEmpty {
                            self.addLog(level: "ERROR", tag: "VM", message: vmErr)
                        }
                    }
                }
            }
        }
    }

    func dispatchClick(viewId: UInt32) {
        guard let ctx = context else { return }
        addLog(level: "DEBUG", tag: "Bridge", message: "Click on view 0x\(String(viewId, radix: 16))")

        Task.detached {
            let result = dx_runtime_dispatch_click(ctx, viewId)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                if result != DX_OK {
                    let errStr = String(cString: dx_result_string(result))
                    self.addLog(level: "WARN", tag: "Bridge", message: "Click dispatch failed: \(errStr)")
                }
                // Refresh render tree
                if let model = dx_runtime_get_render_model(ctx) {
                    self.renderTree = RuntimeBridge.convertRenderModel(model.pointee.root)
                }
            }
        }
    }

    func dispatchLongClick(viewId: UInt32) {
        guard let ctx = context else { return }
        addLog(level: "DEBUG", tag: "Bridge", message: "Long-click on view 0x\(String(viewId, radix: 16))")

        Task.detached {
            let result = dx_runtime_dispatch_long_click(ctx, viewId)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                if result != DX_OK {
                    let errStr = String(cString: dx_result_string(result))
                    self.addLog(level: "WARN", tag: "Bridge", message: "Long-click dispatch failed: \(errStr)")
                }
                // Refresh render tree
                if let model = dx_runtime_get_render_model(ctx) {
                    self.renderTree = RuntimeBridge.convertRenderModel(model.pointee.root)
                }
            }
        }
    }

    func dispatchRefresh(viewId: UInt32) {
        guard let ctx = context else { return }
        addLog(level: "DEBUG", tag: "Bridge", message: "Refresh on view 0x\(String(viewId, radix: 16))")

        Task.detached {
            let result = dx_runtime_dispatch_refresh(ctx, viewId)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                if result != DX_OK {
                    let errStr = String(cString: dx_result_string(result))
                    self.addLog(level: "WARN", tag: "Bridge", message: "Refresh dispatch failed: \(errStr)")
                }
                // Refresh render tree
                if let model = dx_runtime_get_render_model(ctx) {
                    self.renderTree = RuntimeBridge.convertRenderModel(model.pointee.root)
                }
            }
        }
    }

    func updateEditText(viewId: UInt32, text: String) {
        guard let ctx = context else { return }
        text.withCString { cStr in
            dx_runtime_update_edit_text(ctx, viewId, cStr)
        }
    }

    func dispatchBack() {
        guard let ctx = context else { return }
        addLog(level: "DEBUG", tag: "Bridge", message: "Back button pressed")

        Task.detached {
            let result = dx_runtime_dispatch_back(ctx)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                if result != DX_OK {
                    let errStr = String(cString: dx_result_string(result))
                    self.addLog(level: "WARN", tag: "Bridge", message: "Back dispatch failed: \(errStr)")
                }
                // Refresh render tree
                if let model = dx_runtime_get_render_model(ctx) {
                    self.renderTree = RuntimeBridge.convertRenderModel(model.pointee.root)
                }
            }
        }
    }

    func shutdown() {
        if let ctx = context {
            dx_runtime_shutdown(ctx)
            context = nil
        }
        isLoaded = false
        isRunning = false
        renderTree = nil
    }

    // MARK: - Diagnostics

    /// Dump the UI tree hierarchy as a formatted string
    func dumpUITree() -> String {
        guard let ctx = context else { return "(no context)" }
        guard let root = ctx.pointee.ui_root else { return "(no UI tree)" }
        guard let cStr = dx_ui_tree_dump(root) else { return "(dump failed)" }
        let result = String(cString: cStr)
        dx_free(cStr)
        return result
    }

    /// Get heap statistics as a formatted string
    func heapStats() -> String {
        guard let ctx = context, let vm = ctx.pointee.vm else { return "(no VM)" }
        guard let cStr = dx_vm_heap_stats(vm) else { return "(stats failed)" }
        let result = String(cString: cStr)
        dx_free(cStr)
        return result
    }

    /// Get last error detail with register snapshot and stack trace
    func lastErrorDetail() -> String {
        guard let ctx = context, let vm = ctx.pointee.vm else { return "(no VM)" }
        guard let cStr = dx_vm_get_last_error_detail(vm) else { return "(no detail)" }
        let result = String(cString: cStr)
        dx_free(cStr)
        return result
    }

    /// Copy all logs as text (for sharing/debugging)
    func copyLogsToClipboard() -> String {
        return logs.map { entry in
            let ts = Self.logDateFormatter.string(from: entry.timestamp)
            return "[\(ts)] [\(entry.level)] \(entry.tag): \(entry.message)"
        }.joined(separator: "\n")
    }

    // MARK: - Private

    private static let logDateFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    private func enqueuLog(level: String, tag: String, message: String) {
        pendingLogs.append((level, tag, message))

        // Throttle: only flush at most once per run loop iteration
        if !logFlushScheduled {
            logFlushScheduled = true
            // Use DispatchQueue.main.async to batch all logs from this cycle
            DispatchQueue.main.async { [weak self] in
                self?.flushPendingLogs()
            }
        }
    }

    private func flushPendingLogs() {
        logFlushScheduled = false
        let batch = pendingLogs
        pendingLogs = []

        for (level, tag, message) in batch {
            addLog(level: level, tag: tag, message: message)
        }
    }

    private func addLog(level: String, tag: String, message: String) {
        let entry = LogEntry(timestamp: Date(), level: level, tag: tag, message: message)
        logs.append(entry)
        // Keep last entries capped
        if logs.count > Self.maxLogs {
            logs.removeFirst(logs.count - Self.maxLogs)
        }
    }

    nonisolated private static func nodeFromC(_ n: DxRenderNode) -> RenderNode {
        var children: [RenderNode] = []
        if n.child_count > 0, let childPtr = n.children {
            for i in 0..<Int(n.child_count) {
                children.append(nodeFromC(childPtr.advanced(by: i).pointee))
            }
        }

        // Extract image data if present
        var imgData: Data? = nil
        if let ptr = n.image_data, n.image_data_len > 0 {
            imgData = Data(bytes: ptr, count: Int(n.image_data_len))
        }

        let ca = n.constraints
        let anchors = ConstraintAnchors(
            leftToLeft: ca.left_to_left,
            leftToRight: ca.left_to_right,
            rightToRight: ca.right_to_right,
            rightToLeft: ca.right_to_left,
            topToTop: ca.top_to_top,
            topToBottom: ca.top_to_bottom,
            bottomToBottom: ca.bottom_to_bottom,
            bottomToTop: ca.bottom_to_top,
            horizontalBias: ca.horizontal_bias,
            verticalBias: ca.vertical_bias
        )

        return RenderNode(
            type: n.type,
            viewId: n.view_id,
            text: n.text.map { String(cString: $0) },
            hint: n.hint.map { String(cString: $0) },
            orientation: n.orientation,
            textSize: n.text_size,
            width: n.width,
            height: n.height,
            weight: n.weight,
            gravity: n.gravity,
            padding: (n.padding.0, n.padding.1, n.padding.2, n.padding.3),
            margin: (n.margin.0, n.margin.1, n.margin.2, n.margin.3),
            bgColor: n.bg_color,
            textColor: n.text_color,
            isChecked: n.is_checked,
            hasClickListener: n.has_click_listener,
            hasLongClickListener: n.has_long_click_listener,
            hasRefreshListener: n.has_refresh_listener,
            relativeFlags: n.relative_flags,
            relAbove: n.rel_above,
            relBelow: n.rel_below,
            relLeftOf: n.rel_left_of,
            relRightOf: n.rel_right_of,
            constraints: anchors,
            imageData: imgData,
            webURL: n.web_url.map { String(cString: $0) },
            webHTML: n.web_html.map { String(cString: $0) },
            children: children
        )
    }

    nonisolated static func convertRenderModel(_ node: UnsafeMutablePointer<DxRenderNode>?) -> RenderNode? {
        guard let node = node else { return nil }
        return nodeFromC(node.pointee)
    }

    nonisolated private static func convertRenderNode(_ node: inout UnsafeMutablePointer<DxRenderNode>) -> RenderNode? {
        return nodeFromC(node.pointee
        )
    }
}
