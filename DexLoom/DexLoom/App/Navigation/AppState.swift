import SwiftUI

enum AppTab: String, CaseIterable {
    case home = "Home"
    case runtime = "Runtime"
    case logs = "Logs"

    var icon: String {
        switch self {
        case .home: return "house.fill"
        case .runtime: return "play.rectangle.fill"
        case .logs: return "doc.text.fill"
        }
    }
}

@MainActor
final class AppState: ObservableObject {
    @Published var selectedTab: AppTab = .home
    let bridge = RuntimeBridge()
}
