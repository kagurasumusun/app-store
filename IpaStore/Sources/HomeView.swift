// SPDX-License-Identifier: MIT

import SwiftUI

/// Storefront home: ranked Top Free / Top Paid charts for the selected
/// region, in the App Store's Apps-tab shape. Entries are enriched to
/// full catalog records before they reach this view, so rows, GET
/// buttons and product pages behave exactly like search results —
/// browsing needs no sign-in; only downloading does.
struct HomeView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        NavigationStack {
            content
                .navigationTitle(AppTab.apps.title)
                .toolbar {
                    ToolbarItem(placement: .topBarTrailing) {
                        AvatarButton()
                    }
                }
                .navigationDestination(for: StoreApp.self) { app in
                    AppDetailView(app: app)
                }
                .task {
                    await model.loadHome()
                }
                .onChange(of: model.region) { _, _ in
                    Task { await model.loadHome(force: true) }
                }
                .onChange(of: model.platform) { _, _ in
                    Task { await model.loadHome(force: true) }
                }
        }
    }

    @ViewBuilder
    private var content: some View {
        if model.isLoadingHome && model.homeCharts.isEmpty {
            List {
                ForEach(0..<8, id: \.self) { _ in
                    SkeletonAppRow()
                }
            }
            .listStyle(.plain)
        } else if model.homeCharts.isEmpty {
            ContentUnavailableView {
                Label("Couldn't Load Charts", systemImage: "chart.bar.xaxis")
            } description: {
                Text(model.homeError
                     ?? String(localized: "No charts are available in this region."))
            } actions: {
                Button("Retry") {
                    Task { await model.loadHome(force: true) }
                }
                .buttonStyle(.borderedProminent)
            }
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 30) {
                    ForEach(model.homeCharts) { chart in
                        chartSection(chart)
                    }
                }
                .padding(.top, 6)
                .padding(.bottom, 24)
            }
            .refreshable {
                await model.loadHome(force: true)
            }
        }
    }

    private func chartSection(_ chart: HomeChart) -> some View {
        let shown = Array(chart.apps.prefix(9))
        return VStack(alignment: .leading, spacing: 2) {
            HStack(alignment: .firstTextBaseline) {
                Text(chart.title)
                    .font(.system(size: 20, weight: .bold))
                Spacer()
                if chart.apps.count > shown.count {
                    NavigationLink {
                        ChartListView(chart: chart)
                    } label: {
                        Text("See All")
                            .font(.system(size: 15))
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 4)
            ForEach(Array(shown.enumerated()), id: \.element.appID) { index, app in
                ChartRow(rank: index + 1, app: app)
                    .padding(.horizontal, 20)
                if index < shown.count - 1 {
                    Divider().padding(.leading, 130)
                }
            }
        }
    }
}

/// Full ranked chart, reached from "See All".
struct ChartListView: View {
    let chart: HomeChart

    var body: some View {
        List {
            ForEach(Array(chart.apps.enumerated()), id: \.element.appID) { index, app in
                ChartRow(rank: index + 1, app: app)
                    .listRowInsets(EdgeInsets(top: 0, leading: 20, bottom: 0, trailing: 20))
                    .alignmentGuide(.listRowSeparatorLeading) { dimensions in
                        dimensions[.leading] + 110
                    }
            }
        }
        .listStyle(.plain)
        .navigationTitle(chart.title)
        .navigationBarTitleDisplayMode(.inline)
    }
}

/// Rank number + standard app row, as in the App Store's chart lists.
struct ChartRow: View {
    let rank: Int
    let app: StoreApp

    var body: some View {
        HStack(spacing: 12) {
            Text("\(rank)")
                .font(.system(size: 17, weight: .bold))
                .frame(width: 26, alignment: .center)
            AppListRow(app: app)
        }
    }
}
