import SwiftUI

@MainActor
struct AppRouter: View {
    @StateObject private var appState = AppState()

    var body: some View {
        TabView(selection: $appState.selectedTab) {
            HomeView(bridge: appState.bridge)
                .tabItem {
                    Label(AppTab.home.rawValue, systemImage: AppTab.home.icon)
                }
                .tag(AppTab.home)

            RuntimeView(bridge: appState.bridge)
                .tabItem {
                    Label(AppTab.runtime.rawValue, systemImage: AppTab.runtime.icon)
                }
                .tag(AppTab.runtime)

            LogsView(bridge: appState.bridge)
                .tabItem {
                    Label(AppTab.logs.rawValue, systemImage: AppTab.logs.icon)
                }
                .tag(AppTab.logs)
        }
        .tint(Color.dxPrimary)
    }
}
