import SwiftUI
import WebKit

struct RuntimeView: View {
    @ObservedObject var bridge: RuntimeBridge

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                if bridge.isRunning, let root = bridge.renderTree {
                    // Render the Android UI tree
                    ScrollView {
                        AndroidViewRenderer(node: root, bridge: bridge)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding()
                    }
                    .background(Color.white)
                } else if bridge.isRunning {
                    // Running but no render tree yet
                    VStack(spacing: 16) {
                        Image(systemName: "checkmark.circle")
                            .font(.system(size: 64))
                            .foregroundStyle(Color.green)
                        Text("Activity executed successfully")
                            .font(.dxBody)
                            .foregroundStyle(Color.dxText)
                        Text("No visual UI was produced.\nThe app may use Compose or have no setContentView call.")
                            .font(.dxCaption)
                            .foregroundStyle(Color.dxTextSecondary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color.dxBackground)
                } else if bridge.isLoaded {
                    VStack(spacing: 16) {
                        Image(systemName: "play.circle")
                            .font(.system(size: 64))
                            .foregroundStyle(Color.dxTextSecondary)
                        Text("APK loaded. Tap Run to start.")
                            .font(.dxBody)
                            .foregroundStyle(Color.dxTextSecondary)
                        Button("Run Activity") {
                            Task { @MainActor in
                                bridge.run()
                            }
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(Color.dxPrimary)

                        if let error = bridge.errorMessage {
                            VStack(spacing: 8) {
                                Text(error)
                                    .font(.dxCaption)
                                    .foregroundStyle(Color.dxError)
                                    .multilineTextAlignment(.center)
                                Button("Copy Error + Logs") {
                                    let logText = bridge.copyLogsToClipboard()
                                    let full = "Error: \(error)\n\n--- Logs ---\n\(logText)"
                                    UIPasteboard.general.string = full
                                }
                                .font(.dxCaption)
                                .foregroundStyle(Color.dxPrimary)
                            }
                            .padding()
                            .background(Color.dxError.opacity(0.1))
                            .clipShape(RoundedRectangle(cornerRadius: 8))
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color.dxBackground)
                } else {
                    VStack(spacing: 16) {
                        Image(systemName: "cpu")
                            .font(.system(size: 64))
                            .foregroundStyle(Color.dxTextSecondary)
                        Text("No APK loaded")
                            .font(.dxBody)
                            .foregroundStyle(Color.dxTextSecondary)
                        Text("Import an APK from the Home tab")
                            .font(.dxCaption)
                            .foregroundStyle(Color.dxTextSecondary)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color.dxBackground)
                }
            }
            .navigationTitle("Runtime")
        }
    }
}

// MARK: - Color helpers

private func argbColor(_ argb: UInt32) -> Color {
    let a = Double((argb >> 24) & 0xFF) / 255.0
    let r = Double((argb >> 16) & 0xFF) / 255.0
    let g = Double((argb >> 8) & 0xFF) / 255.0
    let b = Double(argb & 0xFF) / 255.0
    return Color(.sRGB, red: r, green: g, blue: b, opacity: a)
}

// MARK: - Android View Renderer

struct AndroidViewRenderer: View {
    let node: RenderNode
    let bridge: RuntimeBridge

    var body: some View {
        if (node.type == DX_VIEW_TEXT_VIEW || node.type == DX_VIEW_BUTTON ||
           node.type == DX_VIEW_EDIT_TEXT || node.type == DX_VIEW_IMAGE_VIEW ||
           node.type == DX_VIEW_SWITCH || node.type == DX_VIEW_CHECKBOX ||
           node.type == DX_VIEW_RADIO_BUTTON || node.type == DX_VIEW_PROGRESS_BAR ||
           node.type == DX_VIEW_SEEK_BAR || node.type == DX_VIEW_RATING_BAR ||
           node.type == DX_VIEW_SPINNER || node.type == DX_VIEW_CHIP ||
           node.type == DX_VIEW_WEB_VIEW || node.type == DX_VIEW_VIEW)
           && node.children.isEmpty {
            leafView
                .applyAndroidStyle(node: node)
                .applyGestures(node: node, bridge: bridge)
        } else {
            // Container views
            containerView
                .applyAndroidStyle(node: node)
                .applyGestures(node: node, bridge: bridge)
        }
    }

    // MARK: - Leaf views

    @ViewBuilder
    private var leafView: some View {
        switch node.type {
        case DX_VIEW_TEXT_VIEW:
            textView

        case DX_VIEW_BUTTON:
            buttonView

        case DX_VIEW_EDIT_TEXT:
            editTextView

        case DX_VIEW_IMAGE_VIEW:
            imageView

        case DX_VIEW_SWITCH:
            switchView

        case DX_VIEW_CHECKBOX:
            checkboxView

        case DX_VIEW_RADIO_BUTTON:
            radioButtonView

        case DX_VIEW_PROGRESS_BAR:
            ProgressView()
                .progressViewStyle(.linear)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)

        case DX_VIEW_SEEK_BAR:
            seekBarView

        case DX_VIEW_RATING_BAR:
            ratingBarView

        case DX_VIEW_SPINNER:
            spinnerView

        case DX_VIEW_CHIP:
            chipView

        case DX_VIEW_WEB_VIEW:
            webViewRendered

        case DX_VIEW_VIEW:
            // Generic <View/> — spacer or divider
            Rectangle()
                .fill(node.bgColor != 0 ? argbColor(node.bgColor) : Color.clear)
                .frame(height: 1)

        default:
            EmptyView()
        }
    }

    // MARK: - Container views

    @ViewBuilder
    private var containerView: some View {
        switch node.type {
        case DX_VIEW_LINEAR_LAYOUT:
            if node.orientation == DX_ORIENTATION_VERTICAL {
                VStack(alignment: .leading, spacing: 0) {
                    childViews
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                HStack(alignment: .center, spacing: 0) {
                    childViews
                }
            }

        case DX_VIEW_CONSTRAINT_LAYOUT:
            ConstraintLayoutView(node: node, bridge: bridge)

        case DX_VIEW_FRAME_LAYOUT:
            ZStack(alignment: .topLeading) {
                childViews
            }
            .frame(maxWidth: .infinity, alignment: .leading)

        case DX_VIEW_RELATIVE_LAYOUT:
            // RelativeLayout: use ZStack with per-child alignment based on relative_flags
            ZStack {
                ForEach(node.children) { child in
                    AndroidViewRenderer(node: child, bridge: bridge)
                        .frame(
                            maxWidth: relativeNeedsFullWidth(child) ? .infinity : nil,
                            maxHeight: relativeNeedsFullHeight(child) ? .infinity : nil,
                            alignment: relativeChildAlignment(child)
                        )
                }
            }
            .frame(maxWidth: .infinity, alignment: .topLeading)

        case DX_VIEW_SCROLL_VIEW:
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    childViews
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

        case DX_VIEW_SWIPE_REFRESH:
            SwipeRefreshContainerView(node: node, bridge: bridge)

        case DX_VIEW_CARD_VIEW:
            VStack(alignment: .leading, spacing: 0) {
                childViews
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.white)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .shadow(color: .black.opacity(0.12), radius: 4, x: 0, y: 2)

        case DX_VIEW_TOOLBAR:
            HStack(spacing: 8) {
                if let text = node.text {
                    Text(text)
                        .font(.headline)
                        .foregroundStyle(.primary)
                }
                Spacer()
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(Color(.systemBackground))

        case DX_VIEW_LIST_VIEW, DX_VIEW_GRID_VIEW:
            // ListView / GridView — vertical list of adapter-provided children
            ScrollView {
                VStack(alignment: .leading, spacing: 1) {
                    childViews
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

        case DX_VIEW_TAB_LAYOUT:
            // TabLayout — render tabs as horizontal buttons
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 0) {
                    childViews
                }
            }
            .frame(maxWidth: .infinity)

        case DX_VIEW_VIEW_PAGER:
            // ViewPager — show first page only (no swipe)
            ZStack(alignment: .topLeading) {
                if let firstChild = node.children.first {
                    AndroidViewRenderer(node: firstChild, bridge: bridge)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

        case DX_VIEW_RADIO_GROUP:
            // RadioGroup — vertical list of radio buttons
            VStack(alignment: .leading, spacing: 4) {
                childViews
            }
            .frame(maxWidth: .infinity, alignment: .leading)

        case DX_VIEW_BOTTOM_NAV:
            // BottomNavigationView — horizontal bar at bottom
            HStack {
                childViews
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 8)
            .background(Color(.systemBackground))
            .overlay(alignment: .top) {
                Divider()
            }

        case DX_VIEW_FAB:
            // FloatingActionButton — circular button
            Button {
                Task { @MainActor in
                    bridge.dispatchClick(viewId: node.viewId)
                }
            } label: {
                Image(systemName: "plus")
                    .font(.title2)
                    .foregroundStyle(.white)
                    .frame(width: 56, height: 56)
                    .background(node.bgColor != 0 ? argbColor(node.bgColor) : Color.blue)
                    .clipShape(Circle())
                    .shadow(color: .black.opacity(0.2), radius: 6, x: 0, y: 3)
            }

        default:
            // DX_VIEW_VIEW_GROUP, DX_VIEW_RECYCLER_VIEW, etc.
            VStack(alignment: .leading, spacing: 0) {
                childViews
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    // MARK: - Child rendering

    @ViewBuilder
    private var childViews: some View {
        ForEach(node.children) { child in
            if child.type.rawValue >= 0 {  // always true, just keeps ForEach happy
                AndroidViewRenderer(node: child, bridge: bridge)
            }
        }
    }

    // MARK: - Specific view builders

    private var textView: some View {
        Text(node.text ?? "")
            .font(.system(size: max(sp(node.textSize), 1)))
            .foregroundStyle(node.textColor != 0 ? argbColor(node.textColor) : .black)
            .multilineTextAlignment(gravityAlignment)
            .frame(maxWidth: .infinity, alignment: gravityFrameAlignment)
    }

    private var buttonView: some View {
        Button {
            Task { @MainActor in
                bridge.dispatchClick(viewId: node.viewId)
            }
        } label: {
            Text(node.text ?? "Button")
                .font(.system(size: max(sp(node.textSize), 1)))
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .frame(maxWidth: .infinity)
                .background(Color.blue.opacity(0.1))
                .foregroundStyle(.blue)
                .clipShape(RoundedRectangle(cornerRadius: 6))
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(Color.blue.opacity(0.3), lineWidth: 1)
                )
        }
    }

    private var editTextView: some View {
        EditTextFieldView(
            initialText: node.text ?? "",
            hint: node.hint ?? "",
            textSize: sp(node.textSize),
            viewId: node.viewId,
            bridge: bridge
        )
    }

    private var imageView: some View {
        Group {
            if let data = node.imageData, let uiImage = UIImage(data: data) {
                Image(uiImage: uiImage)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: .infinity, minHeight: 40)
            } else {
                Image(systemName: "photo")
                    .font(.system(size: 40))
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, minHeight: 60)
            }
        }
    }

    private var switchView: some View {
        HStack {
            if let text = node.text {
                Text(text)
                    .font(.system(size: max(sp(node.textSize), 1)))
            }
            Spacer()
            Image(systemName: node.isChecked ? "switch.2" : "switch.2")
                .foregroundStyle(node.isChecked ? .blue : .gray)
        }
    }

    private var checkboxView: some View {
        HStack(spacing: 8) {
            Image(systemName: node.isChecked ? "checkmark.square.fill" : "square")
                .foregroundStyle(node.isChecked ? .blue : .gray)
            if let text = node.text {
                Text(text)
                    .font(.system(size: max(sp(node.textSize), 1)))
            }
        }
    }

    private var radioButtonView: some View {
        HStack(spacing: 8) {
            Image(systemName: node.isChecked ? "circle.inset.filled" : "circle")
                .foregroundStyle(node.isChecked ? .blue : .gray)
            if let text = node.text {
                Text(text)
                    .font(.system(size: max(sp(node.textSize), 1)))
            }
        }
    }

    private var seekBarView: some View {
        HStack {
            Rectangle()
                .fill(Color.blue)
                .frame(width: 80, height: 4)
            Circle()
                .fill(Color.blue)
                .frame(width: 20, height: 20)
            Rectangle()
                .fill(Color.gray.opacity(0.3))
                .frame(height: 4)
        }
        .frame(maxWidth: .infinity, minHeight: 32)
    }

    private var ratingBarView: some View {
        HStack(spacing: 4) {
            ForEach(0..<5, id: \.self) { i in
                Image(systemName: i < 3 ? "star.fill" : "star")
                    .foregroundStyle(i < 3 ? .yellow : .gray.opacity(0.4))
            }
        }
    }

    private var spinnerView: some View {
        HStack {
            Text(node.text ?? "Select...")
                .foregroundStyle(node.text != nil ? .primary : .secondary)
            Spacer()
            Image(systemName: "chevron.down")
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(
            RoundedRectangle(cornerRadius: 4)
                .stroke(Color.gray.opacity(0.4), lineWidth: 1)
        )
    }

    private var chipView: some View {
        Text(node.text ?? "Chip")
            .font(.system(size: max(sp(node.textSize), 12)))
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(
                Capsule()
                    .fill(node.bgColor != 0 ? argbColor(node.bgColor) : Color.gray.opacity(0.15))
            )
            .overlay(
                Capsule()
                    .stroke(Color.gray.opacity(0.3), lineWidth: 1)
            )
    }

    private var webViewRendered: some View {
        WebViewWrapper(url: node.webURL, html: node.webHTML)
            .frame(maxWidth: .infinity, minHeight: 200)
            .frame(height: node.height > 0 ? dp(node.height) : 300)
    }

    // MARK: - Gravity helpers

    private var gravityAlignment: TextAlignment {
        let h = node.gravity & 0x07  // horizontal gravity bits
        if h == 1 { return .center }       // CENTER_HORIZONTAL
        if h == 5 { return .trailing }     // RIGHT / END
        return .leading
    }

    private var gravityFrameAlignment: Alignment {
        let h = node.gravity & 0x07
        if h == 1 { return .center }
        if h == 5 { return .trailing }
        return .leading
    }

    // MARK: - RelativeLayout helpers

    /// Compute SwiftUI alignment for a child inside a RelativeLayout ZStack
    private func relativeChildAlignment(_ child: RenderNode) -> Alignment {
        let f = child.relativeFlags

        // centerInParent takes priority
        if f & UInt16(DX_REL_CENTER_IN_PARENT) != 0 {
            return .center
        }

        // Determine horizontal alignment
        let left   = f & UInt16(DX_REL_ALIGN_PARENT_LEFT) != 0
        let right  = f & UInt16(DX_REL_ALIGN_PARENT_RIGHT) != 0
        let centerH = f & UInt16(DX_REL_CENTER_HORIZONTAL) != 0

        // Determine vertical alignment
        let top    = f & UInt16(DX_REL_ALIGN_PARENT_TOP) != 0
        let bottom = f & UInt16(DX_REL_ALIGN_PARENT_BOTTOM) != 0
        let centerV = f & UInt16(DX_REL_CENTER_VERTICAL) != 0

        let h: HorizontalAlignment
        if centerH || (left && right) {
            h = .center
        } else if right {
            h = .trailing
        } else {
            h = .leading   // default or explicit left
        }

        let v: VerticalAlignment
        if centerV || (top && bottom) {
            v = .center
        } else if bottom {
            v = .bottom
        } else {
            v = .top       // default or explicit top
        }

        return Alignment(horizontal: h, vertical: v)
    }

    /// Whether this child should stretch horizontally (centerHorizontal or both left+right)
    private func relativeNeedsFullWidth(_ child: RenderNode) -> Bool {
        let f = child.relativeFlags
        let left  = f & UInt16(DX_REL_ALIGN_PARENT_LEFT) != 0
        let right = f & UInt16(DX_REL_ALIGN_PARENT_RIGHT) != 0
        return (left && right) || child.width == -1 // match_parent
    }

    /// Whether this child should stretch vertically (centerVertical or both top+bottom)
    private func relativeNeedsFullHeight(_ child: RenderNode) -> Bool {
        let f = child.relativeFlags
        let top    = f & UInt16(DX_REL_ALIGN_PARENT_TOP) != 0
        let bottom = f & UInt16(DX_REL_ALIGN_PARENT_BOTTOM) != 0
        return (top && bottom) || child.height == -1 // match_parent
    }
}

// MARK: - dp/sp/px to iOS points conversion
//
// Android dp (density-independent pixels) and iOS points are both defined
// relative to a 160dpi baseline. On iOS, 1pt = 1/163" (effectively 160dpi),
// so 1dp = 1pt regardless of screen scale (@2x, @3x).
//
// The C layer (dx_ui_decode_dimension) already converts all dimension units
// (px, dp, sp, pt, in, mm) to iOS-point-equivalent values before they reach
// Swift, so these functions apply no additional scaling. They exist as named
// conversion points so the mapping is explicit and can be adjusted if needed
// (e.g., for user-configurable text scaling).

/// Scale factor applied to all dp values. 1.0 is correct for standard iOS displays.
private let dpScale: CGFloat = 1.0

/// Scale factor for sp (text) values. Increase for accessibility large-text support.
private let spScale: CGFloat = 1.0

/// Convert Android dp to iOS points.
private func dp(_ value: Int32) -> CGFloat {
    CGFloat(value) * dpScale
}

/// Convert Android sp (text size) to iOS points.
private func sp(_ value: Float) -> CGFloat {
    CGFloat(value) * spScale
}

// MARK: - Android style modifier

private struct AndroidStyleModifier: ViewModifier {
    let node: RenderNode

    func body(content: Content) -> some View {
        content
            .applyLayoutSize(width: node.width, height: node.height, weight: node.weight)
            .padding(.leading, dp(node.padding.0))
            .padding(.top, dp(node.padding.1))
            .padding(.trailing, dp(node.padding.2))
            .padding(.bottom, dp(node.padding.3))
            .background(node.bgColor != 0 ? argbColor(node.bgColor) : Color.clear)
            .padding(.leading, dp(node.margin.0))
            .padding(.top, dp(node.margin.1))
            .padding(.trailing, dp(node.margin.2))
            .padding(.bottom, dp(node.margin.3))
    }
}

private struct LayoutSizeModifier: ViewModifier {
    let width: Int32
    let height: Int32
    let weight: Float

    func body(content: Content) -> some View {
        content
            .frame(
                minWidth: widthMin, idealWidth: nil, maxWidth: widthMax,
                minHeight: heightMin, idealHeight: nil, maxHeight: heightMax
            )
    }

    // width: -1 match_parent -> maxWidth: .infinity
    //        -2 wrap_content -> natural size (nil)
    //        >0 specific dp  -> fixed width (converted via dp())
    private var widthMin: CGFloat? {
        if width > 0 { return dp(width) }
        return nil
    }
    private var widthMax: CGFloat? {
        if width == -1 || weight > 0 { return .infinity }   // match_parent or weighted
        if width > 0 { return dp(width) }
        return nil  // wrap_content
    }
    private var heightMin: CGFloat? {
        if height > 0 { return dp(height) }
        return nil
    }
    private var heightMax: CGFloat? {
        if height == -1 { return .infinity }
        if height > 0 { return dp(height) }
        return nil  // wrap_content
    }
}

extension View {
    fileprivate func applyLayoutSize(width: Int32, height: Int32, weight: Float) -> some View {
        modifier(LayoutSizeModifier(width: width, height: height, weight: weight))
    }
}

extension View {
    fileprivate func applyAndroidStyle(node: RenderNode) -> some View {
        modifier(AndroidStyleModifier(node: node))
    }
}

/// Applies tap and long-press gestures to views that have registered listeners.
/// Button and FAB views already handle clicks via SwiftUI Button, so they are excluded
/// from the tap gesture to avoid double-firing.
private struct GestureModifier: ViewModifier {
    let node: RenderNode
    let bridge: RuntimeBridge

    /// View types that already handle clicks via their own SwiftUI Button wrapper
    private var isButtonType: Bool {
        node.type == DX_VIEW_BUTTON || node.type == DX_VIEW_FAB
    }

    func body(content: Content) -> some View {
        content
            .applyTapGesture(
                hasListener: node.hasClickListener && !isButtonType,
                viewId: node.viewId,
                bridge: bridge
            )
            .applyLongPressGesture(
                hasListener: node.hasLongClickListener,
                viewId: node.viewId,
                bridge: bridge
            )
    }
}

extension View {
    fileprivate func applyGestures(node: RenderNode, bridge: RuntimeBridge) -> some View {
        modifier(GestureModifier(node: node, bridge: bridge))
    }

    @ViewBuilder
    fileprivate func applyTapGesture(hasListener: Bool, viewId: UInt32, bridge: RuntimeBridge) -> some View {
        if hasListener {
            self.contentShape(Rectangle())
                .onTapGesture {
                    Task { @MainActor in
                        bridge.dispatchClick(viewId: viewId)
                    }
                }
        } else {
            self
        }
    }

    @ViewBuilder
    fileprivate func applyLongPressGesture(hasListener: Bool, viewId: UInt32, bridge: RuntimeBridge) -> some View {
        if hasListener {
            self.onLongPressGesture {
                Task { @MainActor in
                    bridge.dispatchLongClick(viewId: viewId)
                }
            }
        } else {
            self
        }
    }
}

/// SwipeRefreshLayout rendered as a ScrollView with pull-to-refresh support
private struct SwipeRefreshContainerView: View {
    let node: RenderNode
    let bridge: RuntimeBridge
    @State private var isRefreshing = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                ForEach(node.children) { child in
                    AndroidViewRenderer(node: child, bridge: bridge)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .refreshable {
            isRefreshing = true
            await MainActor.run {
                bridge.dispatchRefresh(viewId: node.viewId)
            }
            // Brief delay so the spinner is visible
            try? await Task.sleep(nanoseconds: 500_000_000)
            isRefreshing = false
        }
    }
}

// MARK: - ConstraintLayout solver

/// A simplified ConstraintLayout renderer.
/// For each child, determines horizontal and vertical positioning based on constraint anchors.
/// Supports: anchoring to parent edges, centering (with bias), and anchoring to sibling edges.
/// Uses GeometryReader + explicit offsets to place children within a ZStack.
private let kConstraintParent: UInt32 = 0xFFFFFFFF

private struct ConstraintLayoutView: View {
    let node: RenderNode
    let bridge: RuntimeBridge

    var body: some View {
        GeometryReader { geo in
            let parentW = geo.size.width
            let parentH = geo.size.height

            ZStack(alignment: .topLeading) {
                ForEach(node.children) { child in
                    let solved = solveChild(child, parentSize: geo.size)
                    AndroidViewRenderer(node: child, bridge: bridge)
                        .frame(
                            width: solved.width,
                            height: solved.height
                        )
                        .frame(
                            maxWidth: solved.maxWidth,
                            maxHeight: solved.maxHeight
                        )
                        .alignmentGuide(.leading) { _ in -solved.x }
                        .alignmentGuide(.top) { _ in -solved.y }
                }
            }
            .frame(width: parentW, height: parentH, alignment: .topLeading)
        }
        .frame(maxWidth: .infinity, minHeight: constraintLayoutMinHeight)
    }

    /// Estimate a reasonable minimum height for the ConstraintLayout based on children
    private var constraintLayoutMinHeight: CGFloat {
        // If any child has an explicit height, use the max; otherwise use a sensible default
        var maxH: CGFloat = 0
        for child in node.children {
            if child.height > 0 {
                maxH = max(maxH, dp(child.height) + dp(child.margin.1) + dp(child.margin.3))
            } else {
                maxH = max(maxH, 48) // minimum per child
            }
        }
        // For ConstraintLayout with match_parent height, don't restrict
        if node.height == -1 { return maxH }
        if node.height > 0 { return dp(node.height) }
        // wrap_content: sum visible children heights as rough estimate
        var total: CGFloat = 0
        for child in node.children {
            if child.height > 0 {
                total += dp(child.height) + dp(child.margin.1) + dp(child.margin.3)
            } else {
                total += 48
            }
        }
        return max(total, maxH)
    }

    /// Solved position and size for a child within the ConstraintLayout
    struct SolvedPosition {
        var x: CGFloat = 0
        var y: CGFloat = 0
        var width: CGFloat? = nil
        var height: CGFloat? = nil
        var maxWidth: CGFloat? = nil
        var maxHeight: CGFloat? = nil
    }

    private func solveChild(_ child: RenderNode, parentSize: CGSize) -> SolvedPosition {
        let c = child.constraints
        let parentW = parentSize.width
        let parentH = parentSize.height
        let marginL = dp(child.margin.0)
        let marginT = dp(child.margin.1)
        let marginR = dp(child.margin.2)
        let marginB = dp(child.margin.3)

        var result = SolvedPosition()

        // Determine child intrinsic width
        let childW: CGFloat? = child.width > 0 ? dp(child.width) : nil
        let childH: CGFloat? = child.height > 0 ? dp(child.height) : nil

        // --- Horizontal axis ---
        let leftEdge = resolveLeftEdge(c, parentW: parentW)
        let rightEdge = resolveRightEdge(c, parentW: parentW)

        if c.isCenteredH {
            // Constrained on both sides
            if let le = leftEdge, let re = rightEdge {
                let availableW = re - le - marginL - marginR
                if child.width == 0 || child.width == -1 {
                    // match_constraint (0dp) or match_parent: stretch to fill
                    result.x = le + marginL
                    result.width = max(availableW, 0)
                } else if let cw = childW {
                    // Fixed size: center with bias
                    let slack = availableW - cw
                    result.x = le + marginL + slack * CGFloat(c.horizontalBias)
                    result.width = cw
                } else {
                    // wrap_content: center with bias
                    // We don't know intrinsic width, so use maxWidth and let SwiftUI handle it
                    result.x = le + marginL
                    result.maxWidth = max(availableW, 0)
                    // Adjust with bias by offsetting: for default 0.5 bias, center
                    // For wrap_content centered, just center in the available space
                }
            }
        } else if c.isLeftOnly {
            if let le = leftEdge {
                result.x = le + marginL
            }
            if let cw = childW { result.width = cw }
        } else if c.isRightOnly {
            if let re = rightEdge {
                if let cw = childW {
                    result.x = re - marginR - cw
                    result.width = cw
                } else {
                    // wrap_content aligned right - approximate
                    result.x = max(re - marginR - 100, 0)
                }
            }
        } else {
            // No horizontal constraints - default to left edge
            result.x = marginL
            if let cw = childW { result.width = cw }
        }

        // --- Vertical axis ---
        let topEdge = resolveTopEdge(c, parentH: parentH)
        let bottomEdge = resolveBottomEdge(c, parentH: parentH)

        if c.isCenteredV {
            if let te = topEdge, let be = bottomEdge {
                let availableH = be - te - marginT - marginB
                if child.height == 0 || child.height == -1 {
                    // match_constraint or match_parent: stretch
                    result.y = te + marginT
                    result.height = max(availableH, 0)
                } else if let ch = childH {
                    let slack = availableH - ch
                    result.y = te + marginT + slack * CGFloat(c.verticalBias)
                    result.height = ch
                } else {
                    result.y = te + marginT
                    result.maxHeight = max(availableH, 0)
                }
            }
        } else if c.isTopOnly {
            if let te = topEdge {
                result.y = te + marginT
            }
            if let ch = childH { result.height = ch }
        } else if c.isBottomOnly {
            if let be = bottomEdge {
                if let ch = childH {
                    result.y = be - marginB - ch
                    result.height = ch
                } else {
                    result.y = max(be - marginB - 48, 0)
                }
            }
        } else {
            // No vertical constraints - default to top
            result.y = marginT
            if let ch = childH { result.height = ch }
        }

        return result
    }

    /// Resolve the left anchor edge position in parent coordinates
    private func resolveLeftEdge(_ c: ConstraintAnchors, parentW: CGFloat) -> CGFloat? {
        if c.leftToLeft == kConstraintParent { return 0 }
        if c.leftToRight == kConstraintParent { return parentW }
        if c.leftToLeft != 0 {
            // Anchored to left edge of sibling - find sibling's right boundary
            // For simplicity, treat sibling constraints as approximate center positions
            return findSiblingLeftEdge(c.leftToLeft)
        }
        if c.leftToRight != 0 {
            return findSiblingRightEdge(c.leftToRight)
        }
        return nil
    }

    private func resolveRightEdge(_ c: ConstraintAnchors, parentW: CGFloat) -> CGFloat? {
        if c.rightToRight == kConstraintParent { return parentW }
        if c.rightToLeft == kConstraintParent { return 0 }
        if c.rightToRight != 0 {
            return findSiblingRightEdge(c.rightToRight)
        }
        if c.rightToLeft != 0 {
            return findSiblingLeftEdge(c.rightToLeft)
        }
        return nil
    }

    private func resolveTopEdge(_ c: ConstraintAnchors, parentH: CGFloat) -> CGFloat? {
        if c.topToTop == kConstraintParent { return 0 }
        if c.topToBottom == kConstraintParent { return parentH }
        if c.topToTop != 0 {
            return findSiblingTopEdge(c.topToTop)
        }
        if c.topToBottom != 0 {
            return findSiblingBottomEdge(c.topToBottom)
        }
        return nil
    }

    private func resolveBottomEdge(_ c: ConstraintAnchors, parentH: CGFloat) -> CGFloat? {
        if c.bottomToBottom == kConstraintParent { return parentH }
        if c.bottomToTop == kConstraintParent { return 0 }
        if c.bottomToBottom != 0 {
            return findSiblingBottomEdge(c.bottomToBottom)
        }
        if c.bottomToTop != 0 {
            return findSiblingTopEdge(c.bottomToTop)
        }
        return nil
    }

    // MARK: - Sibling edge estimation
    // For sibling-to-sibling constraints, we do a simplified single-pass estimation.
    // We estimate sibling positions based on their own constraints to parent only.
    // This handles the most common chains (A→parent, B→A's bottom, etc.)

    private func findSiblingLeftEdge(_ viewId: UInt32) -> CGFloat? {
        // Sibling's left edge is its x position (margin included)
        guard let sibling = node.children.first(where: { $0.viewId == viewId }) else { return nil }
        return dp(sibling.margin.0)
    }

    private func findSiblingRightEdge(_ viewId: UInt32) -> CGFloat? {
        guard let sibling = node.children.first(where: { $0.viewId == viewId }) else { return nil }
        let w: CGFloat = sibling.width > 0 ? dp(sibling.width) : 100 // estimate
        return dp(sibling.margin.0) + w
    }

    private func findSiblingTopEdge(_ viewId: UInt32) -> CGFloat? {
        guard let sibling = node.children.first(where: { $0.viewId == viewId }) else { return nil }
        // If sibling is constrained to parent top, its top is its top margin
        if sibling.constraints.topToTop == kConstraintParent {
            return dp(sibling.margin.1)
        }
        // If sibling is constrained below another sibling, estimate recursively (1 level)
        if sibling.constraints.topToBottom != 0 && sibling.constraints.topToBottom != kConstraintParent {
            if let aboveBottom = findSiblingBottomEdge(sibling.constraints.topToBottom) {
                return aboveBottom + dp(sibling.margin.1)
            }
        }
        return dp(sibling.margin.1)
    }

    private func findSiblingBottomEdge(_ viewId: UInt32) -> CGFloat? {
        guard let sibling = node.children.first(where: { $0.viewId == viewId }) else { return nil }
        let h: CGFloat = sibling.height > 0 ? dp(sibling.height) : 48 // estimate
        if let top = findSiblingTopEdge(viewId) {
            return top + h + dp(sibling.margin.3)
        }
        return dp(sibling.margin.1) + h + dp(sibling.margin.3)
    }
}

// MARK: - EditText interactive view

private struct EditTextFieldView: View {
    let initialText: String
    let hint: String
    let textSize: CGFloat
    let viewId: UInt32
    let bridge: RuntimeBridge

    @State private var text: String = ""
    @FocusState private var isFocused: Bool

    var body: some View {
        TextField(hint, text: $text)
            .font(.system(size: max(textSize, 1)))
            .padding(.horizontal, 8)
            .frame(maxWidth: .infinity)
            .frame(height: 44)
            .background(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(isFocused ? Color.blue : Color.gray.opacity(0.4), lineWidth: isFocused ? 2 : 1)
            )
            .focused($isFocused)
            .onAppear { text = initialText }
            .onChange(of: text) { _, newValue in
                Task { @MainActor in
                    bridge.updateEditText(viewId: viewId, text: newValue)
                }
            }
    }
}

// MARK: - WKWebView wrapper for android.webkit.WebView

private struct WebViewWrapper: UIViewRepresentable {
    let url: String?
    let html: String?

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.defaultWebpagePreferences.allowsContentJavaScript = true
        let webView = WKWebView(frame: .zero, configuration: config)
        webView.isOpaque = false
        webView.backgroundColor = .white
        webView.scrollView.isScrollEnabled = true
        loadContent(into: webView)
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {
        // Reload content when URL or HTML changes
        loadContent(into: webView)
    }

    private func loadContent(into webView: WKWebView) {
        if let html = html, !html.isEmpty {
            webView.loadHTMLString(html, baseURL: nil)
        } else if let urlString = url, !urlString.isEmpty,
                  let parsedURL = URL(string: urlString) {
            webView.load(URLRequest(url: parsedURL))
        } else {
            // No content specified - show blank page with placeholder
            let placeholder = """
            <html><body style="display:flex;align-items:center;justify-content:center;\
            height:100vh;margin:0;font-family:-apple-system;color:#999;">\
            <div style="text-align:center"><div style="font-size:48px">&#127760;</div>\
            <div>WebView</div></div></body></html>
            """
            webView.loadHTMLString(placeholder, baseURL: nil)
        }
    }
}
