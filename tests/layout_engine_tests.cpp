#include "fixture.h"

#include "css/stylesheet.h"
#include "html/parser.h"
#include "layout/layout_engine.h"
#include "platform/browser_core.h"
#include "platform/form_state.h"
#include "render/animation.h"

#include <sstream>

namespace {
class FixedMeasure final : public ITextMeasure {
public:
    float MeasureText(const std::wstring& text, const FontKey&) override {
        return static_cast<float>(text.size()) * 8.f;
    }
    float SpaceWidth(const FontKey&) override { return 4.f; }
    bool ImageIntrinsic(const std::string&, float&, float&) override { return false; }
    void RequestImage(const std::string& url) override { requestedImages.push_back(url); }

    std::vector<std::string> requestedImages;
};

LayoutBox* FindEngineBoxById(LayoutBox* box, const std::string& id) {
    if (!box) return nullptr;
    if (box->node && box->node->attr("id") == id) return box;
    for (auto& child : box->kids)
        if (auto* found = FindEngineBoxById(child.get(), id)) return found;
    return nullptr;
}

LayoutBox* FindEngineBoxByClass(LayoutBox* box, const std::string& cls) {
    if (!box) return nullptr;
    if (box->node) {
        std::istringstream ss(box->node->attr("class"));
        std::string token;
        while (ss >> token) {
            if (token == cls) return box;
        }
    }
    for (auto& child : box->kids)
        if (auto* found = FindEngineBoxByClass(child.get(), cls)) return found;
    return nullptr;
}
} // namespace

TestResult RunLayoutEngineTests() {
    TestResult result;
    auto dom = ParseHtml(
        "<html><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div></div>"
        "<div id=\"column\"><div id=\"c\"></div><div id=\"d\"></div></div></body></html>");
    auto sheet = ParseStylesheet(
        "#row { display:flex; width:200px; gap:10px; }"
        "#row > div { flex:1; height:20px; }"
        "#column { display:flex; flex-direction:column; width:100px; gap:10px; }"
        "#column > div { height:20px; }");
    FixedMeasure measure;
    LayoutInput input;
    input.document = dom.get();
    input.sheet = &sheet;
    input.measure = &measure;
    input.viewportW = 320.f;
    input.viewportH = 480.f;
    auto layout = LayoutDocument(input);
    auto* a = FindEngineBoxById(layout.get(), "a");
    auto* b = FindEngineBoxById(layout.get(), "b");
    auto* c = FindEngineBoxById(layout.get(), "c");
    auto* d = FindEngineBoxById(layout.get(), "d");
    const bool found = a && b && c && d;
    const int rowDelta = found ? static_cast<int>(b->x - a->x + 0.5f) : -1;
    const int rowWidth = found ? static_cast<int>(a->contentW + 0.5f) : -1;
    const int columnDelta = found ? static_cast<int>(d->y - c->y + 0.5f) : -1;
    ExpectEqual("layout-engine/flex-row-grow-gap-and-column-gap",
        std::string(found ? "found " : "missing ") + "rowDelta=" + std::to_string(rowDelta)
            + " rowWidth=" + std::to_string(rowWidth)
            + " columnDelta=" + std::to_string(columnDelta) + "\n",
        "found rowDelta=105 rowWidth=95 columnDelta=30\n",
        result);

    {
        const std::string home = HomePageHtml();
        const size_t styleStart = home.find("<style>");
        const size_t styleEnd = home.find("</style>");
        const std::string css = (styleStart != std::string::npos && styleEnd != std::string::npos)
            ? home.substr(styleStart + 7, styleEnd - styleStart - 7)
            : "";
        auto homeDom = ParseHtml(home);
        auto homeSheet = ParseStylesheet(css);
        FixedMeasure homeMeasure;
        LayoutInput homeInput;
        homeInput.document = homeDom.get();
        homeInput.sheet = &homeSheet;
        homeInput.measure = &homeMeasure;
        homeInput.viewportW = 1000.f;
        homeInput.viewportH = 700.f;
        auto homeLayout = LayoutDocument(homeInput);
        auto* wrap = FindEngineBoxByClass(homeLayout.get(), "w");
        auto* mark = FindEngineBoxByClass(homeLayout.get(), "mark");
        auto* links = FindEngineBoxByClass(homeLayout.get(), "links");
        std::string actual =
            std::string("wrapW=") + (wrap ? std::to_string((int)(wrap->contentW + 0.5f)) : "missing")
            + " wrapX=" + (wrap ? std::to_string((int)(wrap->x + 0.5f)) : "missing")
            + " markDisplay=" + (mark ? std::to_string(mark->style.display) : "missing")
            + " linksDisplay=" + (links ? std::to_string(links->style.display) : "missing")
            + "\n";
        ExpectEqual("layout-engine/homepage-uses-embedded-styles",
            actual,
            "wrapW=720 wrapX=140 markDisplay=1 linksDisplay=0\n",
            result);
    }

    {
        AnimationManager::instance().clear();
        auto animDom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto animSheet = ParseStylesheet(
            "@keyframes grow-box { from { width: 40px; height: 20px; margin-left: 6px; } to { width: 120px; height: 60px; margin-left: 18px; } }"
            "#target { animation: grow-box 1s 10s backwards; width: 10px; height: 10px; }");
        FixedMeasure animMeasure;
        LayoutInput animInput;
        animInput.document = animDom.get();
        animInput.sheet = &animSheet;
        animInput.measure = &animMeasure;
        animInput.viewportW = 320.f;
        animInput.viewportH = 200.f;
        auto animLayout = LayoutDocument(animInput);
        auto* target = FindEngineBoxById(animLayout.get(), "target");
        std::string actual = target
            ? "w=" + std::to_string((int)(target->contentW + 0.5f))
                + " h=" + std::to_string((int)(target->contentH + 0.5f))
                + " x=" + std::to_string((int)(target->x + 0.5f)) + "\n"
            : "missing\n";
        ExpectEqual("layout-engine/css-animation-affects-layout",
            actual,
            "w=40 h=20 x=14\n",
            result);
        AnimationManager::instance().clear();
    }

    {
        auto lazyDom = ParseHtml(
            "<html><body><img src=\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==\" "
            "data-src=\"/images/photo.jpg\"></body></html>");
        auto lazySheet = ParseStylesheet("");
        FixedMeasure lazyMeasure;
        LayoutInput lazyInput;
        lazyInput.document = lazyDom.get();
        lazyInput.sheet = &lazySheet;
        lazyInput.measure = &lazyMeasure;
        lazyInput.viewportW = 320.f;
        lazyInput.viewportH = 480.f;
        lazyInput.baseUrl = "https://example.test/articles/start.html";
        LayoutDocument(lazyInput);
        const std::string requested = lazyMeasure.requestedImages.empty()
            ? "none\n" : lazyMeasure.requestedImages.front() + "\n";
        ExpectEqual("layout-engine/lazy-image-prefers-real-data-src-over-data-placeholder",
            requested,
            "https://example.test/images/photo.jpg\n",
            result);
    }

    {
        auto pictureDom = ParseHtml(
            "<html><body><picture><source srcset=\"/images/cover.png 1x\">"
            "<img id=\"cover\"></picture></body></html>");
        auto pictureSheet = ParseStylesheet("");
        FixedMeasure pictureMeasure;
        LayoutInput pictureInput;
        pictureInput.document = pictureDom.get();
        pictureInput.sheet = &pictureSheet;
        pictureInput.measure = &pictureMeasure;
        pictureInput.viewportW = 320.f;
        pictureInput.viewportH = 480.f;
        pictureInput.baseUrl = "https://example.test/articles/start.html";
        LayoutDocument(pictureInput);
        const std::string requested = pictureMeasure.requestedImages.empty()
            ? "none\n" : pictureMeasure.requestedImages.front() + "\n";
        ExpectEqual("layout-engine/picture-source-provides-image-when-img-has-no-src",
            requested,
            "https://example.test/images/cover.png\n",
            result);
    }

    {
        auto gridDom = ParseHtml(
            "<html><body><div id=\"grid\"><div id=\"g1\"></div><div id=\"g2\"></div>"
            "<div id=\"g3\"></div></div></body></html>");
        auto gridSheet = ParseStylesheet(
            "#grid { display:grid; width:210px; grid-template-columns:1fr 2fr; gap:10px; }"
            "#grid > div { height:20px; }");
        LayoutInput gridInput;
        gridInput.document = gridDom.get();
        gridInput.sheet = &gridSheet;
        gridInput.measure = &measure;
        gridInput.viewportW = 320.f;
        gridInput.viewportH = 480.f;
        auto gridLayout = LayoutDocument(gridInput);
        auto* g1 = FindEngineBoxById(gridLayout.get(), "g1");
        auto* g2 = FindEngineBoxById(gridLayout.get(), "g2");
        auto* g3 = FindEngineBoxById(gridLayout.get(), "g3");
        const bool gridFound = g1 && g2 && g3;
        const int firstWidth = gridFound ? static_cast<int>(g1->contentW + .5f) : -1;
        const int secondX = gridFound ? static_cast<int>(g2->x - g1->x + .5f) : -1;
        const int nextRowY = gridFound ? static_cast<int>(g3->y - g1->y + .5f) : -1;
        ExpectEqual("layout-engine/grid-fr-tracks-and-row-gap",
            std::string(gridFound ? "found " : "missing ")
                + "firstWidth=" + std::to_string(firstWidth)
                + " secondX=" + std::to_string(secondX)
                + " nextRowY=" + std::to_string(nextRowY) + "\n",
            "found firstWidth=67 secondX=77 nextRowY=30\n",
            result);
    }

    {
        std::string giantText(256 * 1024, 'x');
        auto giantDom = ParseHtml("<html><body><p id=\"giant\">" + giantText + "</p></body></html>");
        auto giantSheet = ParseStylesheet("");
        FixedMeasure giantMeasure;
        LayoutInput giantInput;
        giantInput.document = giantDom.get();
        giantInput.sheet = &giantSheet;
        giantInput.measure = &giantMeasure;
        giantInput.viewportW = 640.f;
        giantInput.viewportH = 480.f;
        auto giantLayout = LayoutDocument(giantInput);
        auto* giant = FindEngineBoxById(giantLayout.get(), "giant");
        const bool bounded = giant && !giant->kids.empty()
            && giant->kids.front()->text.size() <= 16 * 1024;
        ExpectEqual("layout-engine/giant-text-is-bounded-before-measurement",
            bounded ? "bounded\n" : "unbounded\n",
            "bounded\n",
            result);
    }

    // Float text-wrap: paragraphs (even nested in a wrapper) must wrap their
    // line boxes to the left of a float:right sidebar, and the wrapper must not
    // stretch to the float's bottom.
    {
        auto fdom = ParseHtml(
            "<html><body><div id=\"box\">"
            "<div id=\"side\"></div>"
            "<div id=\"wrap\"><p id=\"para\">"
            "word word word word word word word word word word word word word word</p></div>"
            "</div></body></html>");
        auto fsheet = ParseStylesheet(
            "#box { width:400px; }"
            "#side { float:right; width:100px; height:200px; }"
            "#wrap, p { margin:0; }");
        FixedMeasure fmeasure;
        LayoutInput fin;
        fin.document = fdom.get();
        fin.sheet = &fsheet;
        fin.measure = &fmeasure;
        fin.viewportW = 800.f;
        fin.viewportH = 600.f;
        auto flayout = LayoutDocument(fin);
        auto* para = FindEngineBoxById(flayout.get(), "para");
        auto* wrap = FindEngineBoxById(flayout.get(), "wrap");
        bool fok = para && wrap && !para->lines.empty();
        // First line must end at or before the float's left edge (x=300).
        bool clamped = fok && (para->lines.front().x + para->lines.front().w) <= 305.f;
        // Wrapper height must reflect only the text (a few lines), not 200px.
        bool notStretched = wrap && wrap->contentH < 150.f;
        ExpectEqual("layout-engine/float-wrap-around-right-sidebar",
            std::string(fok ? "ok" : "missing")
                + " clamped=" + (clamped ? "1" : "0")
                + " notStretched=" + (notStretched ? "1" : "0") + "\n",
            "ok clamped=1 notStretched=1\n",
            result);
    }

    // Auto table layout: shared column widths align cells vertically, the table
    // shrinks to its content, and colspan spans multiple columns.
    {
        auto tdom = ParseHtml(
            "<html><body><table>"
            "<tr><th colspan=\"2\" id=\"hdr\">Header</th></tr>"
            "<tr><td id=\"k1\">Kingdom</td><td id=\"v1\">Animalia</td></tr>"
            "<tr><td id=\"k2\">Phy</td><td id=\"v2\">Chordata longvalue</td></tr>"
            "</table></body></html>");
        auto tsheet = ParseStylesheet(
            "table { border-spacing:0; } td,th { padding:0; margin:0; }");
        FixedMeasure tmeasure;
        LayoutInput tin;
        tin.document = tdom.get();
        tin.sheet = &tsheet;
        tin.measure = &tmeasure;
        tin.viewportW = 1200.f;
        tin.viewportH = 600.f;
        auto tlayout = LayoutDocument(tin);
        auto* k1 = FindEngineBoxById(tlayout.get(), "k1");
        auto* k2 = FindEngineBoxById(tlayout.get(), "k2");
        auto* v1 = FindEngineBoxById(tlayout.get(), "v1");
        auto* hdr = FindEngineBoxById(tlayout.get(), "hdr");
        bool tok = k1 && k2 && v1 && hdr;
        // Column 0 cells share x and width; column 1 starts after column 0.
        bool col0Aligned = tok && (int)(k1->x + .5f) == (int)(k2->x + .5f)
                                && (int)(k1->contentW + .5f) == (int)(k2->contentW + .5f);
        bool col1After = tok && v1->x > k1->x + k1->contentW - 1.f;
        // Header spans both columns: wider than a single column.
        bool spans = tok && hdr->contentW > k1->contentW + 1.f;
        ExpectEqual("layout-engine/table-shared-columns-and-colspan",
            std::string(tok ? "ok" : "missing")
                + " col0Aligned=" + (col0Aligned ? "1" : "0")
                + " col1After=" + (col1After ? "1" : "0")
                + " spans=" + (spans ? "1" : "0") + "\n",
            "ok col0Aligned=1 col1After=1 spans=1\n",
            result);
    }

    // Rowspans reserve their columns in following rows, so later cells land in
    // the same shared columns as the row above instead of sliding left.
    {
        auto rdom = ParseHtml(
            "<html><body><table>"
            "<tr><td id=\"span\" rowspan=\"2\">Tall</td><td id=\"r1\">A</td></tr>"
            "<tr><td id=\"r2\">B</td></tr>"
            "</table></body></html>");
        auto rsheet = ParseStylesheet(
            "table { border-spacing:0; } td { padding:0; margin:0; height:20px; }");
        FixedMeasure rmeasure;
        LayoutInput rin;
        rin.document = rdom.get();
        rin.sheet = &rsheet;
        rin.measure = &rmeasure;
        rin.viewportW = 1200.f;
        rin.viewportH = 600.f;
        auto rlayout = LayoutDocument(rin);
        auto* span = FindEngineBoxById(rlayout.get(), "span");
        auto* r1 = FindEngineBoxById(rlayout.get(), "r1");
        auto* r2 = FindEngineBoxById(rlayout.get(), "r2");
        bool rok = span && r1 && r2;
        bool skipped = rok && (int)(r2->x + .5f) == (int)(r1->x + .5f);
        bool below = rok && r2->y > r1->y + r1->borderBoxH() - 1.f;
        bool tall = rok && span->borderBoxH() >= (r2->y + r2->borderBoxH()) - span->y - 1.f;
        ExpectEqual("layout-engine/table-rowspan-reserves-columns",
            std::string(rok ? "ok" : "missing")
                + " skipped=" + (skipped ? "1" : "0")
                + " below=" + (below ? "1" : "0")
                + " tall=" + (tall ? "1" : "0") + "\n",
            "ok skipped=1 below=1 tall=1\n",
            result);
    }

    // align-items: center positions a short flex item in the middle of the
    // line's cross axis (defined by the tallest item).
    {
        auto adom = ParseHtml(
            "<html><body><div id=\"f\"><div id=\"tall\"></div><div id=\"short\"></div></div></body></html>");
        auto asheet = ParseStylesheet(
            "#f { display:flex; align-items:center; width:200px; }"
            "#tall { width:40px; height:100px; }"
            "#short { width:40px; height:20px; }");
        LayoutInput ain; ain.document = adom.get(); ain.sheet = &asheet;
        ain.measure = &measure; ain.viewportW = 320.f; ain.viewportH = 480.f;
        auto al = LayoutDocument(ain);
        auto* tall = FindEngineBoxById(al.get(), "tall");
        auto* shortBox = FindEngineBoxById(al.get(), "short");
        bool ok = tall && shortBox;
        // short (20px) centered in a 100px line → offset (100-20)/2 = 40px below tall's top.
        int delta = ok ? (int)(shortBox->y - tall->y + .5f) : -1;
        ExpectEqual("layout-engine/flex-align-items-center",
            std::string(ok ? "ok " : "missing ") + "delta=" + std::to_string(delta) + "\n",
            "ok delta=40\n",
            result);
    }

    // width:max-content shrinks a block to its content instead of filling.
    {
        auto mdom = ParseHtml(
            "<html><body><div id=\"mc\">hello</div></body></html>");
        auto msheet = ParseStylesheet("#mc { width:max-content; }");
        LayoutInput min; min.document = mdom.get(); min.sheet = &msheet;
        min.measure = &measure; min.viewportW = 800.f; min.viewportH = 480.f;
        auto ml = LayoutDocument(min);
        auto* mc = FindEngineBoxById(ml.get(), "mc");
        // "hello" = 5 chars * 8px (FixedMeasure) = 40px, not the 800px viewport.
        bool ok = mc && mc->contentW > 1.f && mc->contentW < 100.f;
        ExpectEqual("layout-engine/width-max-content-shrinks-to-content",
            std::string(ok ? "shrunk" : "filled") + "\n",
            "shrunk\n",
            result);
    }

    // display:contents removes the wrapper's own box but preserves children.
    {
        auto cdom = ParseHtml(
            "<html><body><div id=\"wrap\"><p id=\"child\">hello</p></div></body></html>");
        auto csheet = ParseStylesheet(
            "#wrap { display: contents; }"
            "#child { margin:0; height:20px; }");
        LayoutInput cin; cin.document = cdom.get(); cin.sheet = &csheet;
        cin.measure = &measure; cin.viewportW = 320.f; cin.viewportH = 480.f;
        auto cl = LayoutDocument(cin);
        auto* wrap = FindEngineBoxById(cl.get(), "wrap");
        auto* child = FindEngineBoxById(cl.get(), "child");
        ExpectEqual("layout-engine/display-contents-skips-wrapper-keeps-children",
            std::string(!wrap && child ? "contents\n" : "wrapped\n"),
            "contents\n",
            result);
    }

    // display:flow-root establishes a new BFC, so outside floats do not intrude.
    {
        auto frdom = ParseHtml(
            "<html><body><div id=\"box\">"
            "<div id=\"side\"></div>"
            "<div id=\"flow\"><p id=\"para\">word word word word word word word word word word word word</p></div>"
            "</div></body></html>");
        auto frsheet = ParseStylesheet(
            "#box { width:400px; }"
            "#side { float:right; width:100px; height:200px; }"
            "#flow { display: flow-root; margin:0; }"
            "#para { margin:0; }");
        LayoutInput frin; frin.document = frdom.get(); frin.sheet = &frsheet;
        frin.measure = &measure; frin.viewportW = 800.f; frin.viewportH = 600.f;
        auto frl = LayoutDocument(frin);
        auto* para = FindEngineBoxById(frl.get(), "para");
        bool ignoresOutsideFloat = para && !para->lines.empty()
            && (para->lines.front().x + para->lines.front().w) > 305.f;
        ExpectEqual("layout-engine/flow-root-starts-new-bfc",
            ignoresOutsideFloat ? "flow-root\n" : "intruded\n",
            "flow-root\n",
            result);
    }

    // position:sticky should parse as a relative-style fallback, not static.
    {
        auto sdom = ParseHtml("<html><body><div id=\"sticky\"></div></body></html>");
        auto ssheet = ParseStylesheet("#sticky { position: sticky; top: 12px; }");
        LayoutInput sin; sin.document = sdom.get(); sin.sheet = &ssheet;
        sin.measure = &measure; sin.viewportW = 320.f; sin.viewportH = 480.f;
        auto sl = LayoutDocument(sin);
        auto* sticky = FindEngineBoxById(sl.get(), "sticky");
        const bool positioned = sticky && (sticky->style.positionMode == 1 || sticky->style.positionMode == 4)
            && sticky->style.topSet;
        ExpectEqual("layout-engine/sticky-falls-back-to-relative",
            positioned ? "relative\n" : "static\n",
            "relative\n",
            result);
    }

    // Wikipedia-style portal links use a block title followed by an inline-block
    // article count. The count must be laid out on the next line, not flattened
    // onto the same inline baseline as the title.
    {
        auto ldom = ParseHtml(
            "<html><body><a id=\"link\" class=\"link-box\" href=\"#\">"
            "<strong id=\"title\">English</strong>"
            "<small id=\"count\">7,189,000+ <span>articles</span></small>"
            "</a></body></html>");
        auto lsheet = ParseStylesheet(
            ".link-box { display:block; width:156px; text-align:center; }"
            ".link-box strong { display:block; font-size:16px; line-height:20px; }"
            ".link-box small { display:inline-block; font-size:13px; line-height:20px; }");
        LayoutInput lin; lin.document = ldom.get(); lin.sheet = &lsheet;
        lin.measure = &measure; lin.viewportW = 320.f; lin.viewportH = 480.f;
        auto ll = LayoutDocument(lin);
        auto* link = FindEngineBoxById(ll.get(), "link");
        auto* title = FindEngineBoxById(ll.get(), "title");
        auto* count = FindEngineBoxById(ll.get(), "count");
        bool ok = link && title && count
            && count->y >= title->y + title->borderBoxH() - 0.5f
            && link->contentH >= title->marginBoxH() + count->marginBoxH() - 0.5f
            && !count->lines.empty()
            && !count->lines.front().frags.empty()
            && count->lines.front().frags.front().y >= count->contentY() - 0.5f;
        bool anonSafe = link && link->kids.size() >= 2
            && link->kids[1]->anonymous
            && link->kids[1]->node == nullptr
            && !link->kids[1]->href.empty();
        ExpectEqual("layout-engine/inline-block-count-follows-block-title",
            ok && anonSafe ? "stacked\n" : "overlap\n",
            "stacked\n",
            result);
    }

    // UA default: a <dialog> without the open attribute is not rendered.
    {
        auto ddom = ParseHtml(
            "<html><body><dialog id=\"closed\">hidden</dialog>"
            "<dialog id=\"open\" open>visible</dialog></body></html>");
        auto dsheet = ParseStylesheet("");
        LayoutInput din; din.document = ddom.get(); din.sheet = &dsheet;
        din.measure = &measure; din.viewportW = 480.f; din.viewportH = 320.f;
        auto dl = LayoutDocument(din);
        auto* closed = FindEngineBoxById(dl.get(), "closed");
        auto* open = FindEngineBoxById(dl.get(), "open");
        ExpectEqual("layout-engine/dialog-closed-is-display-none-by-default",
            std::string(!closed && open ? "hidden\n" : "visible\n"),
            "hidden\n",
            result);
    }

    // External SVGs should still be treated as requestable replaced images.
    {
        auto sdom = ParseHtml("<html><body><img id=\"mark\" src=\"/static/mark.svg\"></body></html>");
        auto ssheet = ParseStylesheet("");
        FixedMeasure svgMeasure;
        LayoutInput sin; sin.document = sdom.get(); sin.sheet = &ssheet;
        sin.measure = &svgMeasure; sin.viewportW = 320.f; sin.viewportH = 240.f;
        sin.baseUrl = "https://www.wikipedia.org/portal/index.html";
        LayoutDocument(sin);
        const std::string requested = svgMeasure.requestedImages.empty()
            ? "none\n" : svgMeasure.requestedImages.front() + "\n";
        ExpectEqual("layout-engine/external-svg-img-requests-image",
            requested,
            "https://www.wikipedia.org/static/mark.svg\n",
            result);
    }

    // Distilled Wikipedia portal: an absolute logo centered in a relative
    // language cluster and language links placed by percentage offsets.
    {
        auto pdom = ParseHtml(
            "<html><body><div id=\"portal\">"
            "<div id=\"logo\"></div><a id=\"lang1\">English</a><a id=\"lang2\">Deutsch</a>"
            "</div></body></html>");
        auto psheet = ParseStylesheet(
            "body { margin:0; }"
            "#portal { position:relative; width:600px; height:400px; margin-left:auto; margin-right:auto; }"
            "#logo { position:absolute; width:200px; height:180px; left:50%; top:20px; margin-left:-100px; }"
            "#lang1, #lang2 { position:absolute; width:160px; height:40px; }"
            "#lang1 { left:10%; top:120px; }"
            "#lang2 { right:10%; top:120px; }");
        LayoutInput pin; pin.document = pdom.get(); pin.sheet = &psheet;
        pin.measure = &measure; pin.viewportW = 1000.f; pin.viewportH = 600.f;
        auto pl = LayoutDocument(pin);
        auto* portal = FindEngineBoxById(pl.get(), "portal");
        auto* logo = FindEngineBoxById(pl.get(), "logo");
        auto* lang1 = FindEngineBoxById(pl.get(), "lang1");
        auto* lang2 = FindEngineBoxById(pl.get(), "lang2");
        bool ok = portal && logo && lang1 && lang2
            && (int)(portal->x + .5f) == 200
            && (int)(logo->x - portal->contentX() + .5f) == 200
            && (int)(lang1->x - portal->contentX() + .5f) == 60
            && (int)(portal->contentX() + portal->contentW - lang2->x - lang2->borderBoxW() + .5f) == 60;
        ExpectEqual("layout-engine/wikipedia-portal-absolute-cluster",
            ok ? "positioned\n" : "misplaced\n",
            "positioned\n",
            result);
    }

    // Absolutely positioned boxes use the same CSS box model rules as normal
    // blocks: border-box widths include padding/border and percent max-width
    // constraints resolve against the positioned containing block.
    {
        auto bdom = ParseHtml(
            "<html><body><div id=\"host\"><div id=\"abs\"></div><div id=\"cap\"></div></div></body></html>");
        auto bsheet = ParseStylesheet(
            "body { margin:0; }"
            "#host { position:relative; width:400px; height:220px; }"
            "#abs { position:absolute; left:0; top:0; width:200px; height:80px; box-sizing:border-box; padding:20px; border:5px solid black; }"
            "#cap { position:absolute; left:0; top:100px; width:360px; max-width:50%; min-width:25%; height:20px; }");
        LayoutInput bin; bin.document = bdom.get(); bin.sheet = &bsheet;
        bin.measure = &measure; bin.viewportW = 800.f; bin.viewportH = 480.f;
        auto bl = LayoutDocument(bin);
        auto* abs = FindEngineBoxById(bl.get(), "abs");
        auto* cap = FindEngineBoxById(bl.get(), "cap");
        bool ok = abs && cap
            && std::abs(abs->borderBoxW() - 200.f) < 0.5f
            && std::abs(abs->contentW - 150.f) < 0.5f
            && std::abs(abs->borderBoxH() - 80.f) < 0.5f
            && std::abs(abs->contentH - 30.f) < 0.5f
            && std::abs(cap->contentW - 200.f) < 0.5f;
        ExpectEqual("layout-engine/positioned-box-sizing-and-percent-max-width",
            ok ? "sized\n" : "overflow\n",
            "sized\n",
            result);
    }

    // Out-of-flow children must not participate in inline formatting. The
    // Wikipedia portal has an absolute logo and absolute search language picker
    // inside inline-formatting containers; if those consume inline space they
    // shove the wordmark/search text into the wrong line.
    {
        auto odom = ParseHtml(
            "<html><body><div id=\"wrap\">"
            "<img id=\"abs\" src=\"/logo.png\" width=\"80\" height=\"80\">"
            "<span id=\"word\">Wordmark</span>"
            "</div></body></html>");
        auto osheet = ParseStylesheet(
            "#wrap { position:relative; width:200px; text-align:center; }"
            "#abs { position:absolute; top:100px; left:0; width:80px; height:80px; }"
            "#word { display:inline-block; width:100px; height:20px; }");
        FixedMeasure omeasure;
        LayoutInput oin; oin.document = odom.get(); oin.sheet = &osheet;
        oin.measure = &omeasure; oin.viewportW = 320.f; oin.viewportH = 240.f;
        auto ol = LayoutDocument(oin);
        auto* wrap = FindEngineBoxById(ol.get(), "wrap");
        auto* abs = FindEngineBoxById(ol.get(), "abs");
        auto* word = FindEngineBoxById(ol.get(), "word");
        bool ok = wrap && abs && word
            && (int)(word->y - wrap->contentY() + .5f) == 0
            && (int)(wrap->contentH + .5f) == 20
            && (int)(abs->y - wrap->contentY() + .5f) == 100;
        ExpectEqual("layout-engine/out-of-flow-inline-children-do-not-consume-line-space",
            ok ? "out-of-flow\n" : "consumed\n",
            "out-of-flow\n",
            result);
    }

    // A transformed ancestor establishes the containing block for absolutely
    // positioned descendants even when the ancestor itself is not positioned.
    {
        auto tdom = ParseHtml(
            "<html><body><div id=\"wrap\"><div id=\"abs\"></div></div></body></html>");
        auto tsheet = ParseStylesheet(
            "body { margin:0; }"
            "#wrap { transform:translateX(10px); margin-left:100px; width:300px; height:160px; padding:20px; }"
            "#abs { position:absolute; left:25%; top:50%; width:40px; height:20px; }");
        LayoutInput tin; tin.document = tdom.get(); tin.sheet = &tsheet;
        tin.measure = &measure; tin.viewportW = 800.f; tin.viewportH = 480.f;
        auto tl = LayoutDocument(tin);
        auto* wrap = FindEngineBoxById(tl.get(), "wrap");
        auto* abs = FindEngineBoxById(tl.get(), "abs");
        bool ok = wrap && abs
            && std::abs(abs->x - (wrap->contentX() + 75.f)) < 0.5f
            && std::abs(abs->y - (wrap->contentY() + 80.f)) < 0.5f;
        ExpectEqual("layout-engine/transform-establishes-positioned-containing-block",
            ok ? "contained\n" : "viewport\n",
            "contained\n",
            result);
    }

    // Search controls need tag-specific intrinsic sizes rather than every
    // control becoming a generic 140px block.
    {
        auto fdom = ParseHtml(
            "<html><body><form id=\"search\"><input id=\"q\"><button id=\"go\">Search</button>"
            "<select id=\"lang\"><option>EN</option></select></form></body></html>");
        auto fsheet = ParseStylesheet("#search { width:500px; margin:0; } #q { width:300px; }");
        LayoutInput fin; fin.document = fdom.get(); fin.sheet = &fsheet;
        fin.measure = &measure; fin.viewportW = 800.f; fin.viewportH = 400.f;
        auto fl = LayoutDocument(fin);
        auto* q = FindEngineBoxById(fl.get(), "q");
        auto* go = FindEngineBoxById(fl.get(), "go");
        auto* lang = FindEngineBoxById(fl.get(), "lang");
        bool ok = q && go && lang
            && (int)(q->contentW + .5f) == 300
            && go->contentW >= 56.f && go->contentW < 100.f
            && lang->contentW >= 48.f && lang->contentW < 100.f
            && go->contentH >= 28.f && lang->contentH >= 28.f;
        ExpectEqual("layout-engine/form-controls-have-useful-intrinsic-sizes",
            ok ? "sized\n" : "generic\n",
            "sized\n",
            result);
    }

    // Wikipedia's search form includes hidden inputs before/after the visible
    // search controls; they must not create anonymous white input boxes that
    // push the real input/button/select into each other.
    {
        auto hdom = ParseHtml(
            "<html><body><form id=\"search\">"
            "<input type=\"hidden\" id=\"family\" name=\"family\" value=\"wikipedia\">"
            "<div id=\"search-input\"><input id=\"q\" type=\"search\"><div id=\"picker\"><select id=\"lang\"><option>Afrikaans</option></select></div></div>"
            "<button id=\"go\"><i id=\"search-icon\" class=\"sprite svg-search-icon\">Search</i></button>"
            "<input type=\"hidden\" id=\"go-hidden\" value=\"Go\" name=\"go\">"
            "</form></body></html>");
        auto hsheet = ParseStylesheet(
            "#search { width:320px; margin:0; }"
            "#search-input { display:inline-block; position:relative; width:calc(100% - 56px); vertical-align:top; }"
            "#q { width:100%; height:44px; }"
            "#picker { position:absolute; top:0; right:0; width:120px; height:44px; }"
            "#go { display:inline-block; width:56px; min-height:44px; padding:0; vertical-align:top; }"
            ".sprite { display:inline-block; width:22px; height:22px; overflow:hidden; text-indent:-9999px; }");
        LayoutInput hin; hin.document = hdom.get(); hin.sheet = &hsheet;
        hin.measure = &measure; hin.viewportW = 800.f; hin.viewportH = 400.f;
        auto hl = LayoutDocument(hin);
        auto* family = FindEngineBoxById(hl.get(), "family");
        auto* hiddenGo = FindEngineBoxById(hl.get(), "go-hidden");
        auto* wrap = FindEngineBoxById(hl.get(), "search-input");
        auto* q = FindEngineBoxById(hl.get(), "q");
        auto* picker = FindEngineBoxById(hl.get(), "picker");
        auto* go = FindEngineBoxById(hl.get(), "go");
        auto* icon = FindEngineBoxById(hl.get(), "search-icon");
        bool rowOk = !family && !hiddenGo && wrap && q && picker && go
            && go->kind != BoxKind::Replaced
            && icon && icon->kind == BoxKind::InlineBlock
            && std::abs(wrap->contentW - 264.f) < 1.f
            && std::abs(wrap->x - q->x) < 0.5f
            && picker->x >= q->x + q->borderBoxW() - picker->borderBoxW() - 1.f
            && go->x >= wrap->x + wrap->borderBoxW() - 1.f
            && std::abs(go->y - wrap->y) < 1.f;
        ExpectEqual("layout-engine/hidden-inputs-do-not-mangle-search-row",
            rowOk ? "hidden\n" : "visible\n",
            "hidden\n",
            result);
    }

    {
        auto sdom = ParseHtml(
            "<html><body><div id=\"root\"><div id=\"picker\"><select id=\"select\"><option>Afrikaans</option></select></div></div></body></html>");
        auto ssheet = ParseStylesheet(
            "#root{position:relative;width:300px;height:100px;}"
            "#picker{position:absolute;left:0;top:0;width:110px;height:24px;}"
            "#select{width:100%;height:100%;}");
        FixedMeasure smeasure;
        LayoutInput sin; sin.document = sdom.get(); sin.sheet = &ssheet;
        sin.measure = &smeasure; sin.viewportW = 400.f; sin.viewportH = 200.f;
        auto sl = LayoutDocument(sin);
        auto* picker = FindEngineBoxById(sl.get(), "picker");
        auto* select = FindEngineBoxById(sl.get(), "select");
        bool ok = picker && select
            && (int)(picker->contentH + .5f) == 24
            && (int)(select->contentH + .5f) == 24;
        ExpectEqual("layout-engine/percent-height-replaced-child-uses-definite-parent-height",
            ok ? "percent-height\n" : "intrinsic-height\n",
            "percent-height\n",
            result);
    }

    // MediaWiki/Vector increasingly uses logical sizing and inset properties.
    // The search row should resolve those aliases before layout so the input,
    // language picker, and submit button keep a single stable row.
    {
        auto ldom = ParseHtml(
            "<html><body><form id=\"search\">"
            "<div id=\"search-input\"><input id=\"q\" type=\"search\"><div id=\"picker\"><select id=\"lang\"><option>Afrikaans</option></select></div></div>"
            "<button id=\"go\">Search</button>"
            "</form></body></html>");
        auto lsheet = ParseStylesheet(
            "#search { inline-size:360px; margin:0; }"
            "#search-input { display:inline-block; position:relative; inline-size:calc(100% - 64px); block-size:44px; vertical-align:top; }"
            "#q { inline-size:100%; block-size:calc(100% - 0px); }"
            "#picker { position:absolute; inset-inline-end:0; inset-block-start:0; inline-size:120px; block-size:calc(100% - 0px); }"
            "#lang { inline-size:100%; block-size:calc(100% - 0px); }"
            "#go { display:inline-block; inline-size:64px; min-block-size:44px; padding:0; vertical-align:top; }");
        FixedMeasure lmeasure;
        LayoutInput lin; lin.document = ldom.get(); lin.sheet = &lsheet;
        lin.measure = &lmeasure; lin.viewportW = 800.f; lin.viewportH = 400.f;
        auto ll = LayoutDocument(lin);
        auto* wrap = FindEngineBoxById(ll.get(), "search-input");
        auto* q = FindEngineBoxById(ll.get(), "q");
        auto* picker = FindEngineBoxById(ll.get(), "picker");
        auto* lang = FindEngineBoxById(ll.get(), "lang");
        auto* go = FindEngineBoxById(ll.get(), "go");
        bool ok = wrap && q && picker && lang && go
            && std::abs(wrap->contentW - 296.f) < 1.f
            && std::abs(q->contentW - 296.f) < 1.f
            && std::abs(picker->x - (wrap->contentX() + wrap->contentW - picker->borderBoxW())) < 1.f
            && std::abs(lang->contentH - 44.f) < 1.f
            && go->x >= wrap->x + wrap->borderBoxW() - 1.f
            && std::abs(go->y - wrap->y) < 1.f;
        ExpectEqual("layout-engine/wikipedia-search-row-logical-properties",
            ok ? "logical-row\n" : "broken-row\n",
            "logical-row\n",
            result);
    }

    // Dense language lists should wrap into multiple inline lines inside the
    // container instead of overflowing as one massive line.
    {
        auto wdom = ParseHtml(
            "<html><body><ul id=\"langs\">"
            "<li>Afar</li><li>Deutsch</li><li>English</li><li>Español</li><li>Français</li>"
            "<li>Italiano</li><li>Polski</li><li>Português</li><li>Русский</li><li>日本語</li>"
            "</ul></body></html>");
        auto wsheet = ParseStylesheet(
            "#langs { width:240px; margin:0; padding-left:0; }"
            "#langs li { display:inline; margin-right:12px; }");
        LayoutInput win; win.document = wdom.get(); win.sheet = &wsheet;
        win.measure = &measure; win.viewportW = 800.f; win.viewportH = 400.f;
        auto wl = LayoutDocument(win);
        auto* langs = FindEngineBoxById(wl.get(), "langs");
        bool wraps = langs && langs->establishesInline && langs->lines.size() >= 2;
        bool clamped = wraps;
        if (wraps) {
            for (const auto& line : langs->lines)
                clamped = clamped && line.w <= langs->contentW + 0.5f;
        }
        ExpectEqual("layout-engine/dense-inline-language-list-wraps",
            wraps && clamped ? "wrapped\n" : "overflow\n",
            "wrapped\n",
            result);
    }

    // Wikipedia's sister-project tiles are left floats with inline-block link
    // content; the floated cards must pack across the row and their title /
    // tagline spans must stack vertically inside the text column.
    {
        auto pdom = ParseHtml(
            "<html><body><nav id=\"projects\" class=\"other-projects\">"
            "<div id=\"p1\" class=\"other-project\"><a id=\"l1\" class=\"other-project-link\" href=\"#\">"
            "<div class=\"other-project-icon\"><span>i</span></div>"
            "<div id=\"t1\" class=\"other-project-text\"><span id=\"title1\" class=\"other-project-title\">Commons</span>"
            "<span id=\"tag1\" class=\"other-project-tagline\">Free media collection</span></div></a></div>"
            "<div id=\"p2\" class=\"other-project\"><a id=\"l2\" class=\"other-project-link\" href=\"#\">"
            "<div class=\"other-project-icon\"><span>i</span></div>"
            "<div class=\"other-project-text\"><span class=\"other-project-title\">Wikivoyage</span>"
            "<span class=\"other-project-tagline\">Free travel guide</span></div></a></div>"
            "<div id=\"p3\" class=\"other-project\"><a id=\"l3\" class=\"other-project-link\" href=\"#\">"
            "<div class=\"other-project-icon\"><span>i</span></div>"
            "<div class=\"other-project-text\"><span class=\"other-project-title\">Wiktionary</span>"
            "<span class=\"other-project-tagline\">Free dictionary</span></div></a></div>"
            "</nav></body></html>");
        auto psheet = ParseStylesheet(
            ".other-projects{display:inline-block;width:650px;}"
            ".other-project{float:left;position:relative;width:33%;height:9rem;}"
            ".other-project-link{display:inline-block;min-height:50px;width:90%;padding:1em;white-space:nowrap;}"
            ".other-project-icon{display:inline-block;width:50px;text-align:center;}"
            ".other-project-text{display:inline-block;max-width:65%;font-size:1.4rem;vertical-align:middle;white-space:normal;}"
            ".other-project-tagline,.other-project-title{display:block;}"
            ".other-project-tagline{font-size:1.3rem;}");
        LayoutInput pin; pin.document = pdom.get(); pin.sheet = &psheet;
        pin.measure = &measure; pin.viewportW = 1000.f; pin.viewportH = 800.f;
        auto pl = LayoutDocument(pin);
        auto* p1 = FindEngineBoxById(pl.get(), "p1");
        auto* p2 = FindEngineBoxById(pl.get(), "p2");
        auto* p3 = FindEngineBoxById(pl.get(), "p3");
        auto* l1 = FindEngineBoxById(pl.get(), "l1");
        auto* l2 = FindEngineBoxById(pl.get(), "l2");
        auto* l3 = FindEngineBoxById(pl.get(), "l3");
        auto* title = FindEngineBoxById(pl.get(), "title1");
        auto* tagline = FindEngineBoxById(pl.get(), "tag1");
        bool packed = p1 && p2 && p3
            && p2->x >= p1->x + p1->borderBoxW() - 1.f
            && p3->x >= p2->x + p2->borderBoxW() - 1.f
            && std::abs(p2->y - p1->y) < 1.f
            && std::abs(p3->y - p1->y) < 1.f;
        bool childrenMoved = p1 && p2 && p3 && l1 && l2 && l3
            && std::abs(l1->x - p1->x) < 1.f
            && std::abs(l2->x - p2->x) < 1.f
            && std::abs(l3->x - p3->x) < 1.f;
        bool stacked = title && tagline
            && tagline->y >= title->y + title->borderBoxH() - 0.5f;
        ExpectEqual("layout-engine/wikipedia-project-floats-pack-and-stack-text",
            packed && childrenMoved && stacked ? "stable\n" : "overlap\n",
            "stable\n",
            result);
    }

    // Anonymous block wrappers around inline runs must not copy layout-affecting
    // parent styles such as overflow:hidden. Wikipedia's footer uses an
    // overflow-hidden container with floated sidebars followed by an inline
    // project grid; the anonymous inline wrapper still has to see those floats.
    {
        auto fdom = ParseHtml(
            "<html><body><footer id=\"footer\">"
            "<div id=\"side\">hosted by Wikimedia Foundation text text text</div>"
            "<nav id=\"projects\"><a id=\"grid\">Commons Wikivoyage Wikibooks</a></nav>"
            "</footer></body></html>");
        auto fsheet = ParseStylesheet(
            "#footer{overflow:hidden;width:900px;padding:0;}"
            "#side{float:left;width:260px;height:160px;}"
            "#projects{display:inline-block;width:65%;height:120px;}");
        LayoutInput fin; fin.document = fdom.get(); fin.sheet = &fsheet;
        fin.measure = &measure; fin.viewportW = 1000.f; fin.viewportH = 700.f;
        auto fl = LayoutDocument(fin);
        auto* side = FindEngineBoxById(fl.get(), "side");
        auto* projects = FindEngineBoxById(fl.get(), "projects");
        bool placedBesideFloat = side && projects
            && projects->x >= side->x + side->borderBoxW() - 1.f
            && projects->y < side->y + side->borderBoxH();
        ExpectEqual("layout-engine/anonymous-inline-wrapper-sees-footer-floats",
            placedBesideFloat ? "beside\n" : "overlap\n",
            "beside\n",
            result);
    }

    // clear:left also applies to floated boxes. Wikipedia's footer stacks its
    // second left sidebar below the first with float:left; clear:left.
    {
        auto cdom = ParseHtml(
            "<html><body><div id=\"wrap\">"
            "<div id=\"first\"></div><div id=\"second\"></div>"
            "</div></body></html>");
        auto csheet = ParseStylesheet(
            "#wrap{width:500px;}"
            "#first{float:left;width:200px;height:80px;}"
            "#second{float:left;clear:left;width:200px;height:60px;}");
        LayoutInput cin; cin.document = cdom.get(); cin.sheet = &csheet;
        cin.measure = &measure; cin.viewportW = 700.f; cin.viewportH = 400.f;
        auto cl = LayoutDocument(cin);
        auto* first = FindEngineBoxById(cl.get(), "first");
        auto* second = FindEngineBoxById(cl.get(), "second");
        bool cleared = first && second
            && second->y >= first->y + first->borderBoxH() - 0.5f
            && std::abs(second->x - first->x) < 0.5f;
        ExpectEqual("layout-engine/floats-honor-clear-left",
            cleared ? "cleared\n" : "beside\n",
            "cleared\n",
            result);
    }

    // Absolutely positioned boxes with left/right set and auto horizontal
    // margins center in their containing block instead of sticking to left:0.
    {
        auto adom = ParseHtml(
            "<html><body><div id=\"host\"><div id=\"abs\"></div></div></body></html>");
        auto asheet = ParseStylesheet(
            "body{margin:0;} #host{position:relative;width:400px;height:120px;}"
            "#abs{position:absolute;left:0;right:0;top:0;width:200px;height:20px;margin-left:auto;margin-right:auto;}");
        LayoutInput ain; ain.document = adom.get(); ain.sheet = &asheet;
        ain.measure = &measure; ain.viewportW = 800.f; ain.viewportH = 400.f;
        auto al = LayoutDocument(ain);
        auto* abs = FindEngineBoxById(al.get(), "abs");
        bool centered = abs && std::abs(abs->x - 100.f) < 0.5f;
        ExpectEqual("layout-engine/absolute-left-right-auto-margins-center",
            centered ? "centered\n" : "left\n",
            "centered\n",
            result);
    }

    // Inline-blocks use their last inline line baseline, not their bottom edge,
    // when aligning beside normal text.
    {
        auto idom = ParseHtml(
            "<html><body><p id=\"p\"><span id=\"ib\">box</span><span id=\"tail\">text</span></p></body></html>");
        auto isheet = ParseStylesheet(
            "body,p{margin:0;} #ib{display:inline-block;} #tail{display:inline;}");
        LayoutInput iin; iin.document = idom.get(); iin.sheet = &isheet;
        iin.measure = &measure; iin.viewportW = 400.f; iin.viewportH = 200.f;
        auto il = LayoutDocument(iin);
        auto* p = FindEngineBoxById(il.get(), "p");
        float tailY = -1.f;
        if (p && !p->lines.empty()) {
            for (const auto& frag : p->lines.front().frags) {
                if (!frag.text.empty() && frag.text.find(L"text") != std::wstring::npos)
                    tailY = frag.y;
            }
        }
        bool baseline = tailY >= 0.f && std::abs(tailY - p->contentY()) < 1.f;
        ExpectEqual("layout-engine/inline-block-baseline-uses-child-line-baseline",
            baseline ? "baseline\n" : "bottom\n",
            "baseline\n",
            result);
    }

    // Captions sit above table rows, and col/colgroup width hints influence
    // the shared column grid.
    {
        auto tdom = ParseHtml(
            "<html><body><table id=\"table\"><caption id=\"cap\">Caption</caption>"
            "<colgroup><col style=\"width:120px\"><col></colgroup>"
            "<tr><td id=\"a\">A</td><td id=\"b\">B</td></tr></table></body></html>");
        auto tsheet = ParseStylesheet("body{margin:0;} table{border-spacing:0;} caption,td{padding:0;margin:0;}");
        LayoutInput tin; tin.document = tdom.get(); tin.sheet = &tsheet;
        tin.measure = &measure; tin.viewportW = 500.f; tin.viewportH = 300.f;
        auto tl = LayoutDocument(tin);
        auto* cap = FindEngineBoxById(tl.get(), "cap");
        auto* a = FindEngineBoxById(tl.get(), "a");
        auto* b = FindEngineBoxById(tl.get(), "b");
        bool ok = cap && a && b
            && cap->y < a->y
            && std::abs(a->contentW - 120.f) < 1.f
            && b->x >= a->x + a->borderBoxW() - 0.5f;
        ExpectEqual("layout-engine/table-caption-and-col-widths",
            ok ? "structured\n" : "cellified\n",
            "structured\n",
            result);
    }

    // justify-content affects flex row item positions when no flex-grow consumes
    // the free space.
    {
        auto fdom = ParseHtml(
            "<html><body><div id=\"f\"><div id=\"a\"></div><div id=\"b\"></div></div></body></html>");
        auto fsheet = ParseStylesheet(
            "body{margin:0;} #f{display:flex;width:300px;justify-content:space-between;}"
            "#a,#b{width:50px;height:20px;}");
        LayoutInput fin; fin.document = fdom.get(); fin.sheet = &fsheet;
        fin.measure = &measure; fin.viewportW = 500.f; fin.viewportH = 200.f;
        auto fl = LayoutDocument(fin);
        auto* a = FindEngineBoxById(fl.get(), "a");
        auto* b = FindEngineBoxById(fl.get(), "b");
        bool spaced = a && b && std::abs((b->x - a->x) - 250.f) < 0.5f;
        ExpectEqual("layout-engine/flex-justify-content-space-between",
            spaced ? "spaced\n" : "packed\n",
            "spaced\n",
            result);
    }

    // Grid items spanning multiple rows reserve enough row height for the
    // spanned area and push later auto-placed rows below it.
    {
        auto gdom = ParseHtml(
            "<html><body><div id=\"grid\"><div id=\"span\"></div><div id=\"one\"></div><div id=\"two\"></div></div></body></html>");
        auto gsheet = ParseStylesheet(
            "body{margin:0;} #grid{display:grid;width:120px;grid-template-columns:60px 60px;gap:0;}"
            "#span{grid-row:1 / 3;grid-column:1;width:60px;height:80px;}"
            "#one,#two{width:60px;height:20px;}");
        LayoutInput gin; gin.document = gdom.get(); gin.sheet = &gsheet;
        gin.measure = &measure; gin.viewportW = 400.f; gin.viewportH = 300.f;
        auto gl = LayoutDocument(gin);
        auto* span = FindEngineBoxById(gl.get(), "span");
        auto* one = FindEngineBoxById(gl.get(), "one");
        auto* two = FindEngineBoxById(gl.get(), "two");
        bool rows = span && one && two
            && std::abs(one->y - span->y) < 0.5f
            && two->y >= span->y + 40.f - 0.5f;
        ExpectEqual("layout-engine/grid-row-span-distributes-height",
            rows ? "spanned\n" : "collapsed\n",
            "spanned\n",
            result);
    }

    // Hit testing should agree with paint order and clipping: the topmost
    // overlapping link wins, and clipped-away descendants are ignored.
    {
        auto hdom = ParseHtml(
            "<html><body><div id=\"clip\"><a id=\"outside\" href=\"/outside\">outside</a></div>"
            "<div id=\"stack\"><a id=\"front\" href=\"/front\">front</a><a id=\"back\" href=\"/back\">back</a></div></body></html>");
        auto hsheet = ParseStylesheet(
            "body{margin:0;} #clip{position:relative;overflow:hidden;width:100px;height:40px;}"
            "#outside{position:absolute;left:140px;top:0;width:50px;height:20px;display:block;}"
            "#stack{position:relative;width:100px;height:60px;}"
            "#front,#back{position:absolute;left:0;top:0;width:80px;height:40px;display:block;}"
            "#front{z-index:2;} #back{z-index:1;}");
        LayoutInput hin; hin.document = hdom.get(); hin.sheet = &hsheet;
        hin.measure = &measure; hin.viewportW = 400.f; hin.viewportH = 300.f;
        auto hl = LayoutDocument(hin);
        const Node* outside = FormState::hitTestNode(*hl, 150.f, 10.f, 0.f, 0.f);
        const Node* top = FormState::hitTestNode(*hl, 10.f, 50.f, 0.f, 0.f);
        bool ok = (!outside || outside->attr("id") != "outside")
            && top && top->attr("id") == "front";
        ExpectEqual("layout-engine/hit-test-honors-clipping-and-z-order",
            ok ? "paint-order\n" : "dom-order\n",
            "paint-order\n",
            result);
    }

    // Canvas's width/height attributes set its drawing-buffer (intrinsic)
    // size, defaulting to 300x150 when absent, and it composites via the
    // "__canvas__" replacedUrl marker rather than an image fetch.
    {
        auto cdom = ParseHtml(
            "<html><body>"
            "<canvas id=\"plain\"></canvas>"
            "<canvas id=\"sized\" width=\"640\" height=\"480\" style=\"width:100px\"></canvas>"
            "</body></html>");
        auto csheet = ParseStylesheet("body{margin:0;}");
        LayoutInput cin; cin.document = cdom.get(); cin.sheet = &csheet;
        cin.measure = &measure; cin.viewportW = 400.f; cin.viewportH = 300.f;
        auto cl = LayoutDocument(cin);
        auto* plain = FindEngineBoxById(cl.get(), "plain");
        auto* sized = FindEngineBoxById(cl.get(), "sized");
        std::string actual;
        if (plain) {
            actual += (plain->kind == BoxKind::Replaced ? "replaced " : "other ");
            actual += plain->replacedUrl + " ";
            actual += std::to_string((int)plain->intrinsicW) + "x"
                + std::to_string((int)plain->intrinsicH) + "\n";
        } else {
            actual += "missing\n";
        }
        if (sized) {
            actual += std::to_string((int)(sized->contentW + 0.5f)) + "x"
                + std::to_string((int)(sized->contentH + 0.5f)) + "\n";
        } else {
            actual += "missing\n";
        }
        ExpectEqual("layout-engine/canvas-intrinsic-size-and-replaced-marker",
            actual,
            "replaced __canvas__ 300x150\n"
            "100x75\n",
            result);
    }
    return result;
}
