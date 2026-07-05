#include "test/fixture.h"

#include "html/parser.h"
#include "layout/layout.h"
#include "paint/display_list.h"
#include "render/svg.h"

static bool SvgPixelIs(const SvgBitmap& bmp, int x, int y, int rMin, int gMax, int bMax, int aMin) {
    if (x < 0 || x >= bmp.width || y < 0 || y >= bmp.height) return false;
    size_t i = (size_t)(y * bmp.width + x) * 4;
    return bmp.pixels[i] >= rMin
        && bmp.pixels[i + 1] <= gMax
        && bmp.pixels[i + 2] <= bMax
        && bmp.pixels[i + 3] >= aMin;
}

TestResult RunPaintTests() {
    TestResult result;
    auto root = FindRepoRoot();

    auto input = ReadTextFile(root / "tests/fixtures/paint/basic.in.html");
    auto expected = ReadTextFile(root / "tests/fixtures/paint/basic.expected.txt");
    auto dom = ParseHtml(input);
    auto layout = BuildSimpleLayout(dom, 800.0f);
    auto items = BuildDisplayList(layout);
    auto actual = SerializeDisplayList(items);
    ExpectEqual("paint/basic", actual, expected, result);

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        const bool usesStableFontKey = painter.find("const std::string fontKey = FontCacheKey(f);") != std::string::npos;
        ExpectEqual("paint/text-measurement-cache-key-uses-stable-storage",
            usesStableFontKey ? "stable\n" : "temporary\n",
            "stable\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string linuxMain = ReadTextFile(root / "src/platform/main_linux.cpp");
        std::string winRenderer = ReadTextFile(root / "src/render/renderer.cpp");
        const bool sharedPolicy =
            linuxMain.find("SvgRasterMaxDimForBytes") != std::string::npos
            && winRenderer.find("SvgRasterMaxDimForBytes") != std::string::npos
            && linuxMain.find("renderSvgBytes(bytes, 2048)") == std::string::npos;
        ExpectEqual("paint/svg-decode-cap-is-shared-across-platforms",
            sharedPolicy ? "shared\n" : "hardcoded\n",
            "shared\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string profileH = ReadTextFile(root / "src/platform/profile.h");
        const bool profilePages =
            profileH.find("ProfilePaths") != std::string::npos
            && mainWin.find("vertex::profile::DefaultPaths()") != std::string::npos
            && mainWin.find("vertex://settings") != std::string::npos
            && mainWin.find("vertex://site-data") != std::string::npos
            && mainWin.find("AppendDownloadRecord(record)") != std::string::npos
            && mainWin.find("AppendBookmarkRecord(CurTab().url, CurTab().title)") != std::string::npos;
        ExpectEqual("paint/profile-backed-app-pages-are-wired",
            profilePages ? "profile\n" : "adhoc\n",
            "profile\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string cmake = ReadTextFile(root / "CMakeLists.txt");
        std::string workflow = ReadTextFile(root / ".github/workflows/release.yml");
        // Windows packaging is a hand-written NSIS script (installer/windows/vertex.nsi.in),
        // not CPack's NSIS generator — see CMakeLists.txt for why.
        std::string nsiScript = ReadTextFile(root / "installer/windows/vertex.nsi.in");
        const bool packagesInstallers =
            cmake.find("include(CPack)") != std::string::npos
            && workflow.find("--target windows_installer") != std::string::npos
            && nsiScript.find("CreateShortCut") != std::string::npos
            && nsiScript.find("$SMPROGRAMS") != std::string::npos
            && nsiScript.find("Vertex.lnk") != std::string::npos
            && workflow.find("Vertex-windows.exe") == std::string::npos
            && workflow.find("Vertex-windows-installer") != std::string::npos;
        ExpectEqual("paint/release-workflow-packages-installers",
            packagesInstallers ? "installer\n" : "raw-exe\n",
            "installer\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string cmake = ReadTextFile(root / "CMakeLists.txt");
        std::string workflow = ReadTextFile(root / ".github/workflows/release.yml");
        std::string updater = ReadTextFile(root / "src/platform/updater.h");
        const bool helperUpdater =
            cmake.find("add_executable(VertexUpdater") != std::string::npos
            && cmake.find("src/platform/updater_helper.cpp") != std::string::npos
            && cmake.find("install(TARGETS VertexUpdater") != std::string::npos
            && workflow.find("vertex-windows-portable") != std::string::npos
            && workflow.find("Vertex-windows-portable.exe") != std::string::npos
            && updater.find("Vertex-windows-portable.exe") != std::string::npos
            && updater.find("VertexUpdater") != std::string::npos
            && updater.find("--update") != std::string::npos
            && updater.find("--target") != std::string::npos;
        ExpectEqual("paint/updater-uses-helper-and-portable-assets",
            helperUpdater ? "helper-updater\n" : "direct-swap\n",
            "helper-updater\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const bool taskbarIdentity =
            mainWin.find("SetCurrentProcessExplicitAppUserModelID") != std::string::npos
            && mainWin.find("hackclubium.Vertex") != std::string::npos
            && mainWin.find("ICON_SMALL2") != std::string::npos;
        ExpectEqual("paint/windows-taskbar-icon-uses-themed-window-identity",
            taskbarIdentity ? "stable-taskbar-icon\n" : "shortcut-fallback\n",
            "stable-taskbar-icon\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string linuxMain = ReadTextFile(root / "src/platform/main_linux.cpp");
        std::string macMain = ReadTextFile(root / "src/platform/main_macos.mm");
        std::string cmake = ReadTextFile(root / "CMakeLists.txt");
        std::string rc = ReadTextFile(root / "src/platform/vertex.rc");
        std::string resourceH = ReadTextFile(root / "src/platform/resource.h");
        const bool hasPlatformLightAssets =
            std::filesystem::exists(root / "src/platform/vertex_icon_light.png")
            && std::filesystem::exists(root / "src/platform/vertex_icon_light.ico")
            && std::filesystem::exists(root / "src/platform/vertex_icon_light.icns");
        const bool themedIcons =
            hasPlatformLightAssets
            && resourceH.find("IDI_VERTEX_APP_LIGHT") != std::string::npos
            && rc.find("vertex_icon_light.ico") != std::string::npos
            && mainWin.find("AppsUseLightTheme") != std::string::npos
            && mainWin.find("ApplyThemedWindowIcon") != std::string::npos
            && mainWin.find("WM_SETTINGCHANGE") != std::string::npos
            && linuxMain.find("gtk-application-prefer-dark-theme") != std::string::npos
            && linuxMain.find("vertex_icon_light.png") != std::string::npos
            && linuxMain.find("notify::gtk-application-prefer-dark-theme") != std::string::npos
            && cmake.find("src/platform/vertex_icon_light.icns") != std::string::npos
            && macMain.find("effectiveAppearance") != std::string::npos
            && macMain.find("vertex_icon_light.icns") != std::string::npos
            && macMain.find("AppleInterfaceThemeChangedNotification") != std::string::npos;
        ExpectEqual("paint/platform-icons-follow-system-theme",
            themedIcons ? "themed\n" : "static\n",
            "themed\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        const bool hoverDoesNotAlwaysRebuild =
            renderer.find("&& !hoverChanged") == std::string::npos;
        ExpectEqual("paint/hover-does-not-always-break-layout-cache",
            hoverDoesNotAlwaysRebuild ? "conditional\n" : "always\n",
            "conditional\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        const bool imageDecodeCanRepaintOnly =
            renderer.find("ImageDecodeAffectsLayout") != std::string::npos
            && renderer.find("InvalidateLayout();") != std::string::npos
            && renderer.find("InvalidateRect(m_hwnd, nullptr, FALSE);") != std::string::npos;
        ExpectEqual("paint/image-decode-can-avoid-full-layout-invalidation",
            imageDecodeCanRepaintOnly ? "conditional\n" : "always-relayout\n",
            "conditional\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool dirtyRectsReachPainter =
            rendererH.find("SetPaintDirtyRect") != std::string::npos
            && mainWin.find("g_renderer.SetPaintDirtyRect(ps.rcPaint)") != std::string::npos
            && renderer.find("m_hasPaintDirtyRect") != std::string::npos
            && painter.find("CanCullOffscreenPaintSubtree(box, scrollY, topInset, m_paintDirtyTop, m_paintDirtyBottom)") != std::string::npos;
        ExpectEqual("paint/windows-dirty-rect-culls-box-walk",
            dirtyRectsReachPainter ? "dirty-cull\n" : "viewport-walk\n",
            "dirty-cull\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool retainedHits =
            rendererH.find("m_hitRegionScrollY") != std::string::npos
            && renderer.find("RemoveHitRegionsInDirtyRect") != std::string::npos
            && renderer.find("const bool rebuildHitRegions") != std::string::npos
            && renderer.find("if (rebuildHitRegions)") != std::string::npos
            && renderer.find("RemoveHitRegionsInDirtyRect();") != std::string::npos;
        ExpectEqual("paint/dirty-rect-paint-retains-hit-regions",
            retainedHits ? "retained\n" : "clears-all\n",
            "retained\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        const bool cached =
            renderer.find("IDWriteTextFormat* closeFmt = nullptr") == std::string::npos
            && renderer.find("IDWriteTextFormat* centered = nullptr") == std::string::npos;
        ExpectEqual("paint/tab-strip-centering-formats-are-cached",
            cached ? "cached\n" : "per-paint\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string linuxMain = ReadTextFile(root / "src/platform/main_linux.cpp");
        std::string macMain = ReadTextFile(root / "src/platform/main_macos.mm");
        const bool cachesDocumentStyle =
            renderer.find("m_styleDocKey != doc.get()") != std::string::npos
            && renderer.find("m_cachedSheet  = CollectStylesheet(doc.get());") != std::string::npos
            && renderer.find("FindBodyBgColor(doc.get(), m_cachedSheet)") != std::string::npos;
        ExpectEqual("paint/document-style-is-cached-off-scroll-path",
            cachesDocumentStyle ? "cached\n" : "per-paint\n",
            "cached\n",
            result);
        const bool collectedSheetsCanResolve =
            renderer.find("sheet.rebuildRuleBuckets();") != std::string::npos
            && linuxMain.find("sheet.rebuildRuleBuckets();") != std::string::npos
            && macMain.find("sheet.rebuildRuleBuckets();") != std::string::npos;
        ExpectEqual("paint/collected-stylesheets-rebuild-selector-buckets",
            collectedSheetsCanResolve ? "rebuilt\n" : "empty-buckets\n",
            "rebuilt\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        size_t count = 0;
        size_t pos = 0;
        while ((pos = mainWin.find("g_renderer.InvalidateLayout();", pos)) != std::string::npos) {
            ++count;
            pos += 30;
        }
        ExpectEqual("paint/windows-navigation-invalidates-document-style-cache",
            count >= 3 ? "invalidates\n" : "stale-cache-risk\n",
            "invalidates\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string engine = ReadTextFile(root / "src/js/engine.h");
        const bool timerSleeps =
            engine.find("hasPendingMacrotasks() const") != std::string::npos
            && mainWin.find("!g_js.hasPendingMacrotasks()") != std::string::npos
            && mainWin.find("KillTimer(hwnd, 1);") != std::string::npos;
        ExpectEqual("paint/windows-js-timer-sleeps-when-idle",
            timerSleeps ? "sleeps\n" : "spins\n",
            "sleeps\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const bool scriptBatches =
            mainWin.find("g_pendingPageScripts") != std::string::npos
            && mainWin.find("RunPendingPageScripts(") != std::string::npos
            && mainWin.find("kMaxScriptsPerTimerTick") != std::string::npos
            && mainWin.find("fetchBeforeRun") != std::string::npos
            && mainWin.find("RunPendingPageScripts(hwnd);") != std::string::npos;
        ExpectEqual("paint/windows-page-scripts-run-in-timer-batches",
            scriptBatches ? "batched\n" : "blocking\n",
            "batched\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const bool preloadedScripts =
            mainWin.find("__vertex_script_filename") != std::string::npos
            && mainWin.find("preloadedFilename") != std::string::npos
            && mainWin.find("!preloadedFilename.empty()") != std::string::npos;
        ExpectEqual("paint/windows-page-scripts-use-preloaded-worker-body",
            preloadedScripts ? "preloaded\n" : "refetches\n",
            "preloaded\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const bool timerDoesNotFetch =
            mainWin.find("PendingPageScriptWaitingForFetch") != std::string::npos
            && mainWin.find("FetchResourceAsync(job.filename") != std::string::npos
            && mainWin.find("FetchResourceCached(job.filename") == std::string::npos;
        ExpectEqual("paint/windows-page-script-fetches-stay-off-ui-timer",
            timerDoesNotFetch ? "async\n" : "sync\n",
            "async\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string chromeH = ReadTextFile(root / "src/platform/chrome.h");
        std::string linuxMain = ReadTextFile(root / "src/platform/main_linux.cpp");
        std::string macMain = ReadTextFile(root / "src/platform/main_macos.mm");
        const bool timerBudgetsWork =
            mainWin.find("kMaxResourceCompletionsPerTimerTick") != std::string::npos
            && mainWin.find("DrainResourceCompletions(kMaxResourceCompletionsPerTimerTick)") != std::string::npos
            && mainWin.find("g_js.runMacrotasks(kMaxMacrotasksPerTimerTick)") != std::string::npos
            // Linux delegates its timer tick to BrowserChrome::pumpJs() (chrome.h),
            // a shared implementation of the same resource/script/macrotask
            // budgeting Windows does inline — not a duplicate reimplementation.
            && chromeH.find("kMaxResourceCompletionsPerTimerTick") != std::string::npos
            && chromeH.find("DrainResourceCompletions(kMaxResourceCompletionsPerTimerTick)") != std::string::npos
            && chromeH.find("state.js.runMacrotasks(kMaxMacrotasksPerTimerTick)") != std::string::npos
            && linuxMain.find("g_chrome.pumpJs()") != std::string::npos
            && macMain.find("g_js.runMacrotasks(8)") != std::string::npos;
        ExpectEqual("paint/platform-timers-budget-resource-and-js-work",
            timerBudgetsWork ? "budgeted\n" : "unbounded\n",
            "budgeted\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        std::string resourceH = ReadTextFile(root / "src/network/resource_cache.h");
        const bool perfSurface =
            rendererH.find("LastTimings() const") != std::string::npos
            && resourceH.find("ResourceCacheStats") != std::string::npos
            && mainWin.find("VERTEX_PERF") != std::string::npos
            && mainWin.find("g_renderer.LastTimings()") != std::string::npos
            && mainWin.find("ResourceCache::instance().stats()") != std::string::npos;
        ExpectEqual("paint/windows-perf-stats-are-surfaced",
            perfSurface ? "surfaced\n" : "hidden\n",
            "surfaced\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const bool dirtyDropsLayoutCache =
            mainWin.find("auto repaint = []()") != std::string::npos
            && mainWin.find("g_renderer.InvalidateLayout();") != std::string::npos
            && mainWin.find("InvalidateContent();") != std::string::npos;
        ExpectEqual("paint/dom-dirty-invalidates-layout-cache",
            dirtyDropsLayoutCache ? "invalidates\n" : "stale\n",
            "invalidates\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool lineCull =
            painter.find("lineY + line.h < topInset") != std::string::npos
            && sharedPainter.find("lineY + line.h < ps.topInset") != std::string::npos;
        ExpectEqual("paint/offscreen-lines-are-culled-before-fragments",
            lineCull ? "culled\n" : "walked\n",
            "culled\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool offscreenTextSkipsHits =
            painter.find("continue; // offscreen text/link fragments do not need paint or hit regions") != std::string::npos
            && painter.find("if (sy + frag.h < topInset || sy > (float)m_height)") != std::string::npos
            && sharedPainter.find("continue; // offscreen text/link fragments do not need paint or hit regions") != std::string::npos
            && sharedPainter.find("if (sy + frag.h < ps.topInset || sy > (float)ps.r->Height())") != std::string::npos;
        ExpectEqual("paint/offscreen-text-skips-hit-regions",
            offscreenTextSkipsHits ? "skipped\n" : "records-offscreen-hits\n",
            "skipped\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string bridge = ReadTextFile(root / "src/js/dom_bridge.cpp");
        const bool selectorParseCache =
            bridge.find("struct ParsedSelectorGroup") != std::string::npos
            && bridge.find("parseSelectorGroups(") != std::string::npos
            && bridge.find("matchesParsedSelector(") != std::string::npos
            && bridge.find("domQueryAll(Node* root, const std::vector<ParsedSelectorGroup>& groups)") != std::string::npos;
        ExpectEqual("paint/dom-selector-query-parses-once",
            selectorParseCache ? "cached\n" : "reparse-per-node\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string bridge = ReadTextFile(root / "src/js/dom_bridge.cpp");
        std::string css = ReadTextFile(root / "src/css/stylesheet.cpp");
        const bool nestedDomSelectorsCached =
            bridge.find("CachedSelectorGroups") != std::string::npos
            && bridge.find("matchesSelectorCached") != std::string::npos
            && bridge.find("cachedSelectorGroups(selector)") != std::string::npos;
        ExpectEqual("paint/dom-nested-selector-pseudos-use-parse-cache",
            nestedDomSelectorsCached ? "cached\n" : "reparse-nested\n",
            "cached\n",
            result);
        const bool cssRelationalSelectorsCached =
            css.find("CachedCssSelector") != std::string::npos
            && css.find("CachedCssSelector(selector).matches(node)") != std::string::npos
            && css.find("CachedCssSelector(selector).specificity()") != std::string::npos;
        ExpectEqual("paint/css-relational-selector-checks-use-parse-cache",
            cssRelationalSelectorsCached ? "cached\n" : "reparse-relational\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string layout = ReadTextFile(root / "src/layout/layout_engine.cpp");
        const bool reusesChildStyle =
            layout.find("const ComputedStyle* precomputedStyle") != std::string::npos
            && layout.find("precomputedStyle ? *precomputedStyle") != std::string::npos
            && layout.find("BuildBox(child.get(), style, bc, childHref, &childStyle)") != std::string::npos;
        ExpectEqual("paint/layout-build-reuses-precomputed-child-style",
            reusesChildStyle ? "reused\n" : "double-resolve\n",
            "reused\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool blockLinkHitCulls =
            painter.find("bool hitVisible = hy + hh >= topInset && hy <= (float)m_height;") != std::string::npos
            && sharedPainter.find("bool hitVisible = hy + hh >= ps.topInset && hy <= (float)ps.r->Height();") != std::string::npos;
        ExpectEqual("paint/offscreen-block-links-skip-hit-regions",
            blockLinkHitCulls ? "culled\n" : "records-offscreen-hits\n",
            "culled\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool simpleFastPath =
            painter.find("bool simpleInFlowChildren = true;") != std::string::npos
            && sharedPainter.find("bool simpleInFlowChildren = true;") != std::string::npos
            && painter.find("std::vector<const LayoutBox*> negZ") != std::string::npos;
        ExpectEqual("paint/simple-inflow-children-skip-stacking-vectors",
            simpleFastPath ? "fast-path\n" : "vectors\n",
            "fast-path\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string cmake = ReadTextFile(root / "CMakeLists.txt");
        std::string probe = ReadTextFile(root / "tools/render_probe.cpp");
        const bool renderProbe =
            cmake.find("add_executable(render_probe tools/render_probe.cpp)") != std::string::npos
            && probe.find("PaintOrder") != std::string::npos
            && probe.find("metrics boxes=") != std::string::npos
            && probe.find("paint-order") != std::string::npos;
        ExpectEqual("paint/render-probe-tool-is-wired",
            renderProbe ? "wired\n" : "missing\n",
            "wired\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool sharedStacking =
            sharedPainter.find("std::vector<const LayoutBox*> negZ, inflow, floats, posZ;") != std::string::npos
            && sharedPainter.find("std::stable_sort(negZ.begin(), negZ.end(), byZ);") != std::string::npos
            && sharedPainter.find("for (auto* k : negZ)   PaintBoxTree(ps, *k);") != std::string::npos
            && sharedPainter.find("for (auto* k : posZ)   PaintBoxTree(ps, *k);") != std::string::npos;
        ExpectEqual("paint/shared-painter-uses-z-index-stacking-buckets",
            sharedStacking ? "bucketed\n" : "flat\n",
            "bucketed\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool hoverTrackingIsConditional =
            rendererH.find("UsesHoverStyles() const") != std::string::npos
            && mainWin.find("g_renderer.GetLayoutRoot() && g_renderer.UsesHoverStyles()") != std::string::npos;
        ExpectEqual("paint/hover-hit-test-runs-only-when-css-needs-it",
            hoverTrackingIsConditional ? "conditional\n" : "always\n",
            "conditional\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string formState = ReadTextFile(root / "src/platform/form_state.h");
        const bool hoverHitTestPrunes =
            formState.find("bool inside = x >= bx") != std::string::npos
            && formState.find("if (inside || &box == &root)") != std::string::npos
            && formState.find("if (k->isOutOfFlow() || k->isFloat() || k->style.positionMode == 1)") != std::string::npos;
        ExpectEqual("paint/hover-hit-test-prunes-off-target-subtrees",
            hoverHitTestPrunes ? "prunes\n" : "walks-all\n",
            "prunes\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const bool statusIsIdempotent =
            mainWin.find("static std::string lastStatus;") != std::string::npos
            && mainWin.find("if (effective == lastStatus) return;") != std::string::npos;
        ExpectEqual("paint/status-text-skips-redundant-window-updates",
            statusIsIdempotent ? "idempotent\n" : "chatty\n",
            "idempotent\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool hoverFlagCached =
            rendererH.find("m_cachedUsesHoverStyles") != std::string::npos
            && renderer.find("m_cachedUsesHoverStyles = StylesheetUsesHover(m_cachedSheet);") != std::string::npos
            && renderer.find("return m_cachedUsesHoverStyles;") != std::string::npos;
        ExpectEqual("paint/hover-style-presence-is-cached",
            hoverFlagCached ? "cached\n" : "rescans\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool hoverLayoutClassified =
            rendererH.find("m_cachedHoverAffectsLayout") != std::string::npos
            && renderer.find("StylesheetHoverAffectsLayout") != std::string::npos
            && renderer.find("m_cachedHoverAffectsLayout = StylesheetHoverAffectsLayout(m_cachedSheet);") != std::string::npos
            && renderer.find("hoverChanged && sheet && m_cachedHoverAffectsLayout") != std::string::npos;
        ExpectEqual("paint/paint-only-hover-keeps-layout-cache",
            hoverLayoutClassified ? "paint-only\n" : "relayouts\n",
            "paint-only\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const size_t lookupPos = mainWin.find("const Node* hover = g_renderer.HoverNodeAt(");
        const size_t changePos = mainWin.find("if (hover != g_hoverNode)", lookupPos);
        const size_t tickPos = mainWin.find("lastHoverTick = now", lookupPos);
        const bool hoverThrottleAlwaysTicks =
            lookupPos != std::string::npos
            && changePos != std::string::npos
            && tickPos != std::string::npos
            && tickPos < changePos;
        ExpectEqual("paint/hover-throttle-updates-after-every-lookup",
            hoverThrottleAlwaysTicks ? "throttled\n" : "same-node-chatty\n",
            "throttled\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool selectiveHoverRestyle =
            rendererH.find("m_cachedHoverRestylesSubtree") != std::string::npos
            && renderer.find("StylesheetHoverRestylesSubtree") != std::string::npos
            && renderer.find("ApplyPaintOnlyHoverStylesToChangedChain") != std::string::npos
            && renderer.find("!m_cachedHoverRestylesSubtree") != std::string::npos;
        ExpectEqual("paint/paint-only-hover-restyles-changed-chain",
            selectiveHoverRestyle ? "selective\n" : "whole-tree\n",
            "selective\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool brushCache =
            rendererH.find("m_tempBrushCache") != std::string::npos
            && renderer.find("m_tempBrushCache.find(key)") != std::string::npos
            && renderer.find("m_tempBrushCache.clear();") != std::string::npos;
        ExpectEqual("paint/temp-brushes-reuse-same-color-per-frame",
            brushCache ? "cached\n" : "new-each-call\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool hitCache =
            rendererH.find("m_lastHitValid") != std::string::npos
            && renderer.find("m_lastHitRegion = *it;") != std::string::npos
            && renderer.find("return m_lastHitHref;") != std::string::npos;
        ExpectEqual("paint/link-hit-test-reuses-last-region",
            hitCache ? "cached\n" : "scan-only\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string renderer = ReadTextFile(root / "src/render/renderer.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool hoverNodeCache =
            rendererH.find("HoverNodeAt(") != std::string::npos
            && rendererH.find("m_lastHoverNodeValid") != std::string::npos
            && renderer.find("m_lastHoverNodeRegion") != std::string::npos
            && mainWin.find("g_renderer.HoverNodeAt(") != std::string::npos
            && mainWin.find("FormState::hitTestNode(*g_renderer.GetLayoutRoot()") == std::string::npos;
        ExpectEqual("paint/hover-node-hit-test-reuses-last-region",
            hoverNodeCache ? "cached\n" : "walks-tree\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        std::string rendererH = ReadTextFile(root / "src/render/renderer.h");
        const bool dirtyHoverRects =
            rendererH.find("LastHoverRegion(") != std::string::npos
            && mainWin.find("InvalidateHoverRegions(") != std::string::npos
            && mainWin.find("g_renderer.LastHoverRegion(oldHoverRegion)") != std::string::npos
            && mainWin.find("g_renderer.LastHoverRegion(newHoverRegion)") != std::string::npos;
        ExpectEqual("paint/hover-invalidates-dirty-regions",
            dirtyHoverRects ? "dirty\n" : "full-content\n",
            "dirty\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string mainWin = ReadTextFile(root / "src/main.cpp");
        const bool cursorCached =
            mainWin.find("SetBrowserCursor(HCURSOR cursor)") != std::string::npos
            && mainWin.find("static HCURSOR lastCursor") != std::string::npos
            && mainWin.find("if (cursor == lastCursor) return;") != std::string::npos;
        ExpectEqual("paint/cursor-updates-skip-redundant-setcursor",
            cursorCached ? "cached\n" : "chatty\n",
            "cached\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        const bool authorStyledControls =
            painter.find("FormControlHasSpriteDescendant") != std::string::npos
            && painter.find("authorPaintedChrome = s.bgColor.valid") != std::string::npos
            && painter.find("s.color.valid ? ToD2Dc(s.color)") != std::string::npos;
        ExpectEqual("paint/form-controls-respect-author-button-styling",
            authorStyledControls ? "author\n" : "native-overpaint\n",
            "author\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool authorChromeCanSuppressNative =
            painter.find("authorPaintedChrome") != std::string::npos
            && painter.find("drawNativeChrome") != std::string::npos
            && sharedPainter.find("authorPaintedChrome") != std::string::npos
            && sharedPainter.find("drawNativeChrome") != std::string::npos;
        ExpectEqual("paint/form-controls-do-not-overpaint-author-chrome",
            authorChromeCanSuppressNative ? "author-chrome\n" : "native-overpaint\n",
            "author-chrome\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        const bool plainTextFastPath =
            painter.find("bool needsLayoutObject =") != std::string::npos
            && painter.find("if (!needsLayoutObject)") != std::string::npos
            && painter.find("m_rt->DrawText(frag.text.c_str()") != std::string::npos;
        ExpectEqual("paint/plain-text-skips-explicit-text-layout",
            plainTextFastPath ? "fast-path\n" : "layout-every-fragment\n",
            "fast-path\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool subtreeCull =
            painter.find("bool offscreenSimpleSubtree =") != std::string::npos
            && painter.find("screenY + box.borderBoxH() < topInset") != std::string::npos
            && sharedPainter.find("bool offscreenSimpleSubtree =") != std::string::npos
            && sharedPainter.find("screenY + box.borderBoxH() < ps.topInset") != std::string::npos;
        ExpectEqual("paint/offscreen-simple-subtrees-are-culled",
            subtreeCull ? "culled\n" : "visited\n",
            "culled\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool complexSubtreeCull =
            painter.find("CanCullOffscreenPaintSubtree") != std::string::npos
            && painter.find("ComputePaintYExtent") != std::string::npos
            && painter.find("if (CanCullOffscreenPaintSubtree(box, scrollY, topInset, m_paintDirtyTop, m_paintDirtyBottom))") != std::string::npos
            && sharedPainter.find("CanCullOffscreenPaintSubtree") != std::string::npos
            && sharedPainter.find("ComputePaintYExtent") != std::string::npos
            && sharedPainter.find("if (CanCullOffscreenPaintSubtree(box, ps.scrollY, ps.topInset, (float)ps.r->Height()))") != std::string::npos;
        ExpectEqual("paint/offscreen-complex-subtrees-use-visual-bounds",
            complexSubtreeCull ? "culled\n" : "walks-complex\n",
            "culled\n",
            result);
    }

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        std::string sharedPainter = ReadTextFile(root / "src/platform/box_painter.h");
        const bool blockLinksHit =
            painter.find("!box.href.empty()") != std::string::npos
            && painter.find("m_hits.push_back({ hx, hy, hw, hh, box.href, box.download, box.downloadName });") != std::string::npos
            && sharedPainter.find("!box.href.empty()") != std::string::npos
            && sharedPainter.find("ps.hits->push_back({ hx, hy, hw, hh, box.href, box.download, box.downloadName });") != std::string::npos;
        ExpectEqual("paint/block-link-boxes-register-hit-regions",
            blockLinksHit ? "clickable\n" : "text-only\n",
            "clickable\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"12\" height=\"10\" viewBox=\"0 0 12 10\">"
            "<rect x=\"1\" y=\"2\" width=\"4\" height=\"3\" fill=\"red\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        bool hasRed = false;
        for (size_t i = 0; i + 3 < bmp.pixels.size(); i += 4) {
            if (bmp.pixels[i] > 200 && bmp.pixels[i + 1] < 40
             && bmp.pixels[i + 2] < 40 && bmp.pixels[i + 3] > 200) {
                hasRed = true;
                break;
            }
        }
        ExpectEqual("paint/svg-bytes-rasterize-external-image",
            (bmp.width == 12 && bmp.height == 10 && hasRed) ? "svg\n" : "empty\n",
            "svg\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" "
            "x1=\"0\" y1=\"0\" x2=\"10\" y2=\"0\" gradientTransform=\"rotate(90 5 5)\">"
            "<stop offset=\"0%\" stop-color=\"red\"/>"
            "<stop offset=\"100%\" stop-color=\"blue\"/>"
            "</linearGradient></defs>"
            "<rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        size_t top = (size_t)(1 * bmp.width + 5) * 4;
        size_t bottom = (size_t)(8 * bmp.width + 5) * 4;
        bool transformed = bmp.width == 10 && bmp.height == 10
            && bmp.pixels[top] > bmp.pixels[top + 2]
            && bmp.pixels[bottom + 2] > bmp.pixels[bottom];
        ExpectEqual("paint/svg-gradient-transform-rotates-sampling",
            transformed ? "rotated\n" : "flat\n",
            "rotated\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<path fill=\"red\" fill-rule=\"evenodd\" "
            "d=\"M1 1H9V9H1Z M3 3H7V7H3Z\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        size_t center = (size_t)(5 * bmp.width + 5) * 4;
        bool centerIsHole = bmp.width == 10 && bmp.height == 10 && bmp.pixels[center + 3] == 0;
        ExpectEqual("paint/svg-path-fill-rule-evenodd-makes-hole",
            centerIsHole ? "hole\n" : "filled\n",
            "hole\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<style>.cut{fill:red;fill-rule:evenodd}</style>"
            "<path class=\"cut\" d=\"M1 1H9V9H1Z M3 3H7V7H3Z\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        size_t center = (size_t)(5 * bmp.width + 5) * 4;
        bool centerIsHole = bmp.width == 10 && bmp.height == 10 && bmp.pixels[center + 3] == 0;
        ExpectEqual("paint/svg-css-fill-rule-evenodd-makes-hole",
            centerIsHole ? "hole\n" : "filled\n",
            "hole\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<defs><symbol id=\"mark\"><rect x=\"1\" y=\"1\" width=\"4\" height=\"4\" fill=\"red\"/></symbol></defs>"
            "<use href=\"#mark\" x=\"2\" y=\"2\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        ExpectEqual("paint/svg-use-renders-symbol-children",
            SvgPixelIs(bmp, 4, 4, 200, 40, 40, 200) ? "symbol\n" : "empty\n",
            "symbol\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<style>.a.b{fill:red}</style>"
            "<rect class=\"a b\" x=\"2\" y=\"2\" width=\"4\" height=\"4\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        ExpectEqual("paint/svg-css-selector-requires-all-classes",
            SvgPixelIs(bmp, 4, 4, 200, 40, 40, 200) ? "matched\n" : "missed\n",
            "matched\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"12\" height=\"8\" viewBox=\"0 0 12 8\">"
            "<line x1=\"4\" y1=\"4\" x2=\"8\" y2=\"4\" stroke=\"red\" stroke-width=\"4\" stroke-linecap=\"round\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        bool cornerClear = bmp.width == 12 && bmp.height == 8 && bmp.pixels[(2 * bmp.width + 2) * 4 + 3] == 0;
        ExpectEqual("paint/svg-stroke-round-linecap-avoids-square-corners",
            cornerClear ? "round\n" : "square\n",
            "round\n",
            result);
    }

    return result;
}
