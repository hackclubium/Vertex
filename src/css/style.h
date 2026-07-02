#pragma once
#include <string>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

static constexpr float kCssNotSet = -1e6f;
static constexpr float kCssAuto   = -2.f;

struct CssColor {
    bool  valid = false;
    float r = 0, g = 0, b = 0, a = 1;
};
CssColor ParseCssColor(const std::string& s);

struct ComputedStyle {
    CssColor    color;
    CssColor    bgColor;
    std::string fontFamily;          // "" = default
    std::string backgroundImage;     // URL from background-image: url(...)
    bool     bgColorSet   = false;
    bool     backgroundImageSet = false;
    bool     bgNoRepeat   = false;
    bool     bgFixed      = false;
    // Background placement (CSS sprites). repeat: 0=repeat,1=repeat-x,2=repeat-y,3=no-repeat.
    int      bgRepeat     = 0;
    bool     bgRepeatSet  = false;
    float    bgPosX       = 0;     // position offset; px unless bgPosXPct
    float    bgPosY       = 0;
    bool     bgPosXPct    = false; // bgPosX is a percentage (0..100)
    bool     bgPosYPct    = false;
    bool     bgPosSet     = false;
    int      bgSizeMode   = 0;     // 0=auto, 1=cover, 2=contain, 3=length
    float    bgSizeW      = -1;    // when mode=length (px; -1=auto dimension)
    float    bgSizeH      = -1;
    bool     bgSizeWPct   = false;
    bool     bgSizeHPct   = false;
    bool     bgSizeSet    = false;
    float    fontSize     = 0;
    bool     bold         = false;
    bool     boldSet      = false;
    bool     italic       = false;
    bool     italicSet    = false;
    bool     underline    = false;
    bool     lineThrough  = false;   // text-decoration: line-through
    bool     noUnderline  = false;   // text-decoration: none (overrides link underline)
    // Display: 0=unset, 1=block, 2=inline, 3=none, 4=flex, 5=table, 6=table-cell,
    // 11=grid, 12=flow-root, 13=contents
    int      display      = 0;
    // Box model (kCssNotSet = not set, kCssAuto = auto for margins)
    float    marginTop    = kCssNotSet;
    float    marginRight  = kCssNotSet;
    float    marginBottom = kCssNotSet;
    float    marginLeft   = kCssNotSet;
    float    paddingTop   = -1;
    float    paddingRight = -1;
    float    paddingBottom= -1;
    float    paddingLeft  = -1;
    float    borderWidth  = -1;
    float    borderTopWidth    = -1;
    float    borderRightWidth  = -1;
    float    borderBottomWidth = -1;
    float    borderLeftWidth   = -1;
    CssColor borderColor;
    CssColor borderTopColor;
    CssColor borderRightColor;
    CssColor borderBottomColor;
    CssColor borderLeftColor;
    float    borderRadius = 0;
    // Text
    float    lineHeight      = 0;
    int      textAlign       = 0;      // 0=left, 1=center, 2=right, 3=justify
    bool     textAlignSet    = false;
    float    textIndent      = 0;      // first-line indent (device px before zoom)
    bool     textIndentSet   = false;
    // vertical-align (inline): 0=baseline,1=sub,2=super,3=middle,4=top,5=bottom
    int      verticalAlign    = 0;
    bool     verticalAlignSet = false;
    int      textTransform   = 0;      // 0=none, 1=uppercase, 2=lowercase, 3=capitalize
    bool     textTransformSet= false;
    bool     whiteSpaceNowrap= false;
    bool     whiteSpaceSet   = false;
    bool     whiteSpacePre   = false;
    float    letterSpacing   = 0;    // extra px between characters
    bool     letterSpacingSet= false;
    int      wordBreak       = 0;    // 0=normal, 1=break-all, 2=break-word
    bool     wordBreakSet    = false;
    int      textOverflow    = 0;    // 0=clip, 1=ellipsis
    bool     textOverflowSet = false;
    int      columnCount     = 0;    // 0=auto (no multi-column), >0=number of columns
    float    columnGap       = 16;   // gap between columns (default ~1em)
    bool     columnCountSet  = false;
    float    aspectRatio     = 0;    // width/height ratio (0=auto)
    bool     aspectRatioSet  = false;
    // transition
    float    transitionDuration = 0; // seconds
    std::string transitionProperty; // "all", "opacity", "color", etc.
    bool     transitionSet   = false;
    // outline
    float    outlineWidth    = 0;
    CssColor outlineColor;
    bool     outlineSet      = false;
    // text-shadow
    float    textShadowX     = 0;
    float    textShadowY     = 0;
    float    textShadowBlur  = 0;
    CssColor textShadowColor;
    bool     textShadowSet   = false;
    // Sizing
    float    width        = -1;
    float    widthPercent = -1;
    float    widthCalcPercent = -1;
    float    widthCalcOffset  = 0;
    float    height       = -1;
    float    heightPercent = -1;
    float    maxWidth     = -1;
    float    maxWidthPercent = -1;
    float    minWidth     = -1;
    float    minWidthPercent = -1;
    float    minHeight    = -1;
    float    minHeightPercent = -1;
    float    maxHeight    = -1;
    float    maxHeightPercent = -1;
    // Generated content. This is only meaningful for :before/:after boxes.
    bool     contentSet   = false;
    std::string content;
    // Layout
    int      floatMode    = 0;       // 0=none, 1=left, 2=right
    bool     floatInherit = false;
    int      clearMode    = 0;       // 0=none, 1=left, 2=right, 3=both
    int      positionMode = 0;       // 0=static, 1=relative, 2=absolute, 3=fixed
    int      zIndex       = 0;
    bool     zIndexSet    = false;
    bool     overflowHidden = false;   // legacy: true if hidden/auto/scroll
    bool     overflowSet    = false;
    // 0=visible, 1=hidden, 2=auto, 3=scroll
    int      overflowMode   = 0;
    float    top          = 0;
    float    right        = 0;
    float    bottom       = 0;
    float    left         = 0;
    bool     topSet       = false;
    bool     rightSet     = false;
    bool     bottomSet    = false;
    bool     leftSet      = false;
    bool     topPercent   = false;
    bool     rightPercent = false;
    bool     bottomPercent= false;
    bool     leftPercent  = false;
    // Visibility
    bool     visibilityHidden = false;
    float    opacity          = 1.f;
    bool     opacitySet       = false;
    // object-fit for replaced elements: 0=fill,1=contain,2=cover,3=none,4=scale-down
    int      objectFit        = 0;
    // Intrinsic-sizing keyword for width/height: 0=none,1=min-content,2=max-content,3=fit-content
    int      widthKeyword     = 0;
    int      heightKeyword    = 0;
    // box-sizing: 0=content-box (default), 1=border-box
    int      boxSizing        = 0;
    bool     boxSizingSet     = false;
    // box-shadow: offsetX, offsetY, blurRadius, spreadRadius, color.
    float    shadowX      = 0;
    float    shadowY      = 0;
    float    shadowBlur   = 0;
    float    shadowSpread = 0;
    CssColor shadowColor;
    bool     shadowSet    = false;
    bool     shadowInset  = false;
    // CSS transform (simplified: one translate + one scale + one rotate).
    float    transformTx      = 0;   // translateX (px)
    float    transformTy      = 0;   // translateY (px)
    bool     transformTxPercent = false;
    bool     transformTyPercent = false;
    float    transformScale   = 1;   // scale factor
    float    transformRotate  = 0;   // rotation (degrees)
    bool     transformSet     = false;
    bool     visibilitySet    = false;
    // List style
    bool     listStyleNone    = false;
    bool     listStyleSet     = false;
    // Border-spacing
    float    borderSpacing    = -1;
    // Baseline flex formatting values.
    int      flexDirection   = 0; // 0=row, 1=column
    bool     flexDirectionSet = false;
    float    flexGrow        = 0;
    bool     flexGrowSet     = false;
    float    flexShrink      = 1;  // default 1 per spec
    bool     flexShrinkSet   = false;
    float    flexBasis       = -1; // -1 = auto (falls back to width or max-content)
    bool     flexBasisSet    = false;
    int      flexWrap        = 0;  // 0=nowrap, 1=wrap, 2=wrap-reverse
    bool     flexWrapSet     = false;
    int      alignSelf       = -1; // -1=auto (inherits align-items), 0..3 same as alignItems
    bool     alignSelfSet    = false;
    float    flexGap         = -1;
    // align-items on the cross axis: 0=stretch(default),1=start,2=center,3=end
    int      alignItems      = 0;
    bool     alignItemsSet   = false;
    int      justifyContent  = 0; // 0=start,1=center,2=end,3=space-between
    bool     justifyContentSet = false;
    // Grid tracks remain CSS tokens until their containing block is known.
    std::vector<std::string> gridTemplateColumns;
    bool     gridTemplateColumnsSet = false;
    std::vector<std::string> gridTemplateRows;
    bool     gridTemplateRowsSet = false;
    int      gridColumnStart = 0, gridColumnEnd = 0; // 0 = auto
    int      gridRowStart = 0, gridRowEnd = 0;
    // Linear gradient.
    struct GradientStop { CssColor color; float pos = -1; }; // pos: 0..1, -1=auto
    float    gradientAngle    = 180;  // degrees (0=to top, 90=to right, 180=to bottom)
    std::vector<GradientStop> gradientStops;
    bool     gradientSet      = false;
    // Custom properties stay as tokens until the element has inherited its
    // parent's variables; declarations containing var() are resolved then.
    std::map<std::string, std::string> customProperties;
    std::vector<std::pair<std::string, std::string>> deferredDeclarations;

    bool isDisplayNone()        const { return display == 3; }
    bool isDisplayBlock()       const { return display == 1; }
    bool isDisplayInline()      const { return display == 2; }
    bool isDisplayFlex()        const { return display == 4; }
    bool isDisplayGrid()        const { return display == 11; }
    bool isDisplayTable()       const { return display == 5; }
    bool isDisplayTableCell()   const { return display == 6; }
    bool isDisplayInlineBlock() const { return display == 7; }
    bool isDisplayListItem()    const { return display == 8; }
    bool isDisplayTableRow()    const { return display == 9; }
    bool isDisplayTableRowGroup() const { return display == 10; }
    bool isDisplayFlowRoot()    const { return display == 12; }
    bool isDisplayContents()    const { return display == 13; }
    bool marginTopSet()       const { return marginTop    > kCssNotSet + 1.f; }
    bool marginRightSet()     const { return marginRight  > kCssNotSet + 1.f; }
    bool marginBottomSet()    const { return marginBottom > kCssNotSet + 1.f; }
    bool marginLeftSet()      const { return marginLeft   > kCssNotSet + 1.f; }
    bool isMarginAuto(float v) const { return v > kCssAuto - 0.5f && v < kCssAuto + 0.5f; }

    ComputedStyle inherit(const ComputedStyle& child) const {
        ComputedStyle out = *this;
        if (child.color.valid)       out.color   = child.color;
        if (child.bgColorSet)       { out.bgColor = child.bgColor; out.bgColorSet = true; }
        if (!child.fontFamily.empty())      out.fontFamily      = child.fontFamily;
        if (child.backgroundImageSet) {
            out.backgroundImage = child.backgroundImage;
            out.backgroundImageSet = true;
            out.bgNoRepeat = child.bgNoRepeat;
            out.bgFixed = child.bgFixed;
        }
        if (child.bgRepeatSet) {
            out.bgRepeat = child.bgRepeat; out.bgNoRepeat = (child.bgRepeat == 3);
            out.bgRepeatSet = true;
        }
        if (child.bgPosSet) {
            out.bgPosX = child.bgPosX; out.bgPosY = child.bgPosY;
            out.bgPosXPct = child.bgPosXPct; out.bgPosYPct = child.bgPosYPct;
            out.bgPosSet = true;
        }
        if (child.bgSizeSet) {
            out.bgSizeMode = child.bgSizeMode;
            out.bgSizeW = child.bgSizeW; out.bgSizeH = child.bgSizeH;
            out.bgSizeWPct = child.bgSizeWPct; out.bgSizeHPct = child.bgSizeHPct;
            out.bgSizeSet = true;
        }
        if (child.fontSize > 0)      out.fontSize  = child.fontSize;
        if (child.boldSet)   { out.bold = child.bold; out.boldSet = true; }
        if (child.italicSet) { out.italic = child.italic; out.italicSet = true; }
        if (child.underline)    out.underline   = true;
        if (child.lineThrough)  out.lineThrough = true;
        if (child.noUnderline)  out.noUnderline = true;
        if (child.display != 0)     out.display      = child.display;
        if (child.marginTopSet())    out.marginTop    = child.marginTop;
        if (child.marginRightSet())  out.marginRight  = child.marginRight;
        if (child.marginBottomSet()) out.marginBottom = child.marginBottom;
        if (child.marginLeftSet())   out.marginLeft   = child.marginLeft;
        if (child.paddingTop   >= 0) out.paddingTop    = child.paddingTop;
        if (child.paddingRight >= 0) out.paddingRight  = child.paddingRight;
        if (child.paddingBottom>= 0) out.paddingBottom = child.paddingBottom;
        if (child.paddingLeft  >= 0) out.paddingLeft   = child.paddingLeft;
        if (child.borderWidth       >= 0) out.borderWidth       = child.borderWidth;
        if (child.borderTopWidth    >= 0) out.borderTopWidth    = child.borderTopWidth;
        if (child.borderRightWidth  >= 0) out.borderRightWidth  = child.borderRightWidth;
        if (child.borderBottomWidth >= 0) out.borderBottomWidth = child.borderBottomWidth;
        if (child.borderLeftWidth   >= 0) out.borderLeftWidth   = child.borderLeftWidth;
        if (child.borderColor.valid) out.borderColor   = child.borderColor;
        if (child.borderTopColor.valid) out.borderTopColor = child.borderTopColor;
        if (child.borderRightColor.valid) out.borderRightColor = child.borderRightColor;
        if (child.borderBottomColor.valid) out.borderBottomColor = child.borderBottomColor;
        if (child.borderLeftColor.valid) out.borderLeftColor = child.borderLeftColor;
        if (child.borderRadius > 0)  out.borderRadius  = child.borderRadius;
        if (child.lineHeight   > 0)  out.lineHeight    = child.lineHeight;
        if (child.textAlignSet) { out.textAlign = child.textAlign; out.textAlignSet = true; }
        if (child.textIndentSet) { out.textIndent = child.textIndent; out.textIndentSet = true; }
        if (child.verticalAlignSet) { out.verticalAlign = child.verticalAlign; out.verticalAlignSet = true; }
        if (child.textTransformSet) { out.textTransform = child.textTransform; out.textTransformSet = true; }
        if (child.whiteSpaceSet) { out.whiteSpaceNowrap = child.whiteSpaceNowrap; out.whiteSpacePre = child.whiteSpacePre; out.whiteSpaceSet = true; }
        if (child.letterSpacingSet) { out.letterSpacing = child.letterSpacing; out.letterSpacingSet = true; }
        if (child.wordBreakSet) { out.wordBreak = child.wordBreak; out.wordBreakSet = true; }
        if (child.textOverflowSet) { out.textOverflow = child.textOverflow; out.textOverflowSet = true; }
        if (child.columnCountSet) { out.columnCount = child.columnCount; out.columnGap = child.columnGap; out.columnCountSet = true; }
        if (child.aspectRatioSet) { out.aspectRatio = child.aspectRatio; out.aspectRatioSet = true; }
        if (child.transitionSet) { out.transitionDuration = child.transitionDuration; out.transitionProperty = child.transitionProperty; out.transitionSet = true; }
        if (child.outlineSet) { out.outlineWidth = child.outlineWidth; out.outlineColor = child.outlineColor; out.outlineSet = true; }
        if (child.textShadowSet) { out.textShadowX = child.textShadowX; out.textShadowY = child.textShadowY; out.textShadowBlur = child.textShadowBlur; out.textShadowColor = child.textShadowColor; out.textShadowSet = true; }
        if (child.width        >= 0) { out.width = child.width; out.widthPercent = -1; out.widthCalcPercent = -1; out.widthCalcOffset = 0; }
        if (child.widthPercent >= 0) { out.widthPercent = child.widthPercent; out.width = -1; out.widthCalcPercent = -1; out.widthCalcOffset = 0; }
        if (child.widthCalcPercent >= 0) { out.widthCalcPercent = child.widthCalcPercent; out.widthCalcOffset = child.widthCalcOffset; out.width = -1; out.widthPercent = -1; }
        if (child.height       >= 0) out.height    = child.height;
        if (child.heightPercent >= 0) out.heightPercent = child.heightPercent;
        if (child.maxWidth     >= 0) out.maxWidth  = child.maxWidth;
        if (child.maxWidthPercent >= 0) out.maxWidthPercent = child.maxWidthPercent;
        if (child.minWidth     >= 0) out.minWidth  = child.minWidth;
        if (child.minWidthPercent >= 0) out.minWidthPercent = child.minWidthPercent;
        if (child.minHeight    >= 0) out.minHeight = child.minHeight;
        if (child.minHeightPercent >= 0) out.minHeightPercent = child.minHeightPercent;
        if (child.maxHeight    >= 0) out.maxHeight = child.maxHeight;
        if (child.maxHeightPercent >= 0) out.maxHeightPercent = child.maxHeightPercent;
        if (child.contentSet) { out.contentSet = true; out.content = child.content; }
        if (child.floatMode    != 0) out.floatMode = child.floatMode;
        if (child.floatInherit)      out.floatInherit = true;
        if (child.clearMode    != 0) out.clearMode = child.clearMode;
        if (child.positionMode != 0) out.positionMode = child.positionMode;
        if (child.zIndexSet) { out.zIndex = child.zIndex; out.zIndexSet = true; }
        if (child.overflowSet)  { out.overflowHidden = child.overflowHidden; out.overflowSet = true; out.overflowMode = child.overflowMode; }
        if (child.topSet)    { out.top    = child.top;    out.topSet    = true; out.topPercent    = child.topPercent; }
        if (child.rightSet)  { out.right  = child.right;  out.rightSet  = true; out.rightPercent  = child.rightPercent; }
        if (child.bottomSet) { out.bottom = child.bottom; out.bottomSet = true; out.bottomPercent = child.bottomPercent; }
        if (child.leftSet)   { out.left   = child.left;   out.leftSet   = true; out.leftPercent   = child.leftPercent; }
        if (child.visibilitySet) { out.visibilityHidden = child.visibilityHidden; out.visibilitySet = true; }
        if (child.opacitySet) { out.opacity = child.opacity; out.opacitySet = true; }
        if (child.objectFit != 0) out.objectFit = child.objectFit;
        if (child.widthKeyword != 0) out.widthKeyword = child.widthKeyword;
        if (child.heightKeyword != 0) out.heightKeyword = child.heightKeyword;
        if (child.boxSizingSet) { out.boxSizing = child.boxSizing; out.boxSizingSet = true; }
        if (child.shadowSet) {
            out.shadowX = child.shadowX; out.shadowY = child.shadowY;
            out.shadowBlur = child.shadowBlur; out.shadowSpread = child.shadowSpread;
            out.shadowColor = child.shadowColor; out.shadowSet = true; out.shadowInset = child.shadowInset;
        }
        if (child.transformSet) {
            out.transformTx = child.transformTx; out.transformTy = child.transformTy;
            out.transformTxPercent = child.transformTxPercent;
            out.transformTyPercent = child.transformTyPercent;
            out.transformScale = child.transformScale; out.transformRotate = child.transformRotate;
            out.transformSet = true;
        }
        if (child.listStyleSet) { out.listStyleNone = child.listStyleNone; out.listStyleSet = true; }
        if (child.borderSpacing >= 0) out.borderSpacing = child.borderSpacing;
        if (child.flexDirectionSet) {
            out.flexDirection = child.flexDirection;
            out.flexDirectionSet = true;
        }
        if (child.alignItemsSet) { out.alignItems = child.alignItems; out.alignItemsSet = true; }
        if (child.justifyContentSet) { out.justifyContent = child.justifyContent; out.justifyContentSet = true; }
        if (child.flexGrowSet) {
            out.flexGrow = child.flexGrow;
            out.flexGrowSet = true;
        }
        if (child.flexShrinkSet) { out.flexShrink = child.flexShrink; out.flexShrinkSet = true; }
        if (child.flexBasisSet) { out.flexBasis = child.flexBasis; out.flexBasisSet = true; }
        if (child.flexWrapSet) { out.flexWrap = child.flexWrap; out.flexWrapSet = true; }
        if (child.alignSelfSet) { out.alignSelf = child.alignSelf; out.alignSelfSet = true; }
        if (child.gradientSet) { out.gradientAngle = child.gradientAngle; out.gradientStops = child.gradientStops; out.gradientSet = true; }
        if (child.flexGap >= 0) out.flexGap = child.flexGap;
        if (child.gridTemplateColumnsSet) { out.gridTemplateColumns = child.gridTemplateColumns; out.gridTemplateColumnsSet = true; }
        if (child.gridTemplateRowsSet) { out.gridTemplateRows = child.gridTemplateRows; out.gridTemplateRowsSet = true; }
        if (child.gridColumnStart) out.gridColumnStart = child.gridColumnStart;
        if (child.gridColumnEnd) out.gridColumnEnd = child.gridColumnEnd;
        if (child.gridRowStart) out.gridRowStart = child.gridRowStart;
        if (child.gridRowEnd) out.gridRowEnd = child.gridRowEnd;
        for (const auto& [name, value] : child.customProperties)
            out.customProperties[name] = value;
        out.deferredDeclarations.insert(out.deferredDeclarations.end(),
            child.deferredDeclarations.begin(), child.deferredDeclarations.end());
        return out;
    }
};
