//
//  ContentView.swift
//  Tether
//
//  Created by Zack Bartel on 4/12/26.
//

import SwiftUI
import TetherFramework

struct ContentView: View {
    @Environment(TetherViewModel.self) private var viewModel

    var body: some View {
        @Bindable var vm = viewModel

        TabView(selection: $vm.selectedTab) {
            Tab("Dashboard", systemImage: "antenna.radiowaves.left.and.right", value: AppTab.dashboard) {
                DashboardView()
            }

            Tab("Clipboard", systemImage: "clipboard", value: AppTab.clipboard) {
                ClipboardView()
            }

            Tab("Files", systemImage: "arrow.up.arrow.down", value: AppTab.files) {
                FilesView()
            }

            Tab("Settings", systemImage: "gearshape", value: AppTab.settings) {
                SettingsView()
            }
        }
        .tint(.teal)
        .sheet(isPresented: $vm.showPairingSheet) {
            PairingView()
                .environment(viewModel)
                .interactiveDismissDisabled()
        }
        .alert("Error", isPresented: .init(
            get: { viewModel.errorMessage != nil },
            set: { if !$0 { vm.errorMessage = nil } }
        )) {
            Button("OK") { vm.errorMessage = nil }
        } message: {
            Text(viewModel.errorMessage ?? "")
        }
    }
}

#Preview {
    ContentView()
        .environment(TetherViewModel())
}
