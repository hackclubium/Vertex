#include "js/canvas_bridge.h"
#include "js/dom_bridge.h"
#include "js/canvas_surface.h"
#include "css/style.h"
#include "html/dom.h"

namespace {

void addCtxMethod(VM& vm, JsObject* ctx, const std::string& name, NativeFn fn) {
    auto* fnObj = vm.gc().newNativeFunction(std::move(fn), name);
    ctx->setProp(name, JsValue::object(fnObj));
}

float NumArg(const std::vector<JsValue>& args, size_t i, float def = 0.f) {
    return args.size() > i ? (float)args[i].toNumber() : def;
}

bool BoolArg(const std::vector<JsValue>& args, size_t i, bool def = false) {
    return args.size() > i ? args[i].toBool() : def;
}

// Reads the ctx object's current fillStyle/globalAlpha (or strokeStyle) and
// pushes them into the surface right before a draw call. There is no live
// property-set hook for plain JS objects (only DOM-wrapper objects get one),
// so this "current value at call time" read is how ctx.fillStyle = '...'
// actually takes effect.
void SyncFillStyle(JsObject* ctx, ICanvasSurface* surf) {
    CssColor c = ParseCssColor(ctx->getProp("fillStyle").toString());
    surf->SetFillStyle(c);
    surf->SetGlobalAlpha((float)ctx->getProp("globalAlpha").toNumber());
}

void SyncStrokeStyle(JsObject* ctx, ICanvasSurface* surf) {
    CssColor c = ParseCssColor(ctx->getProp("strokeStyle").toString());
    surf->SetStrokeStyle(c);
    surf->SetLineWidth((float)ctx->getProp("lineWidth").toNumber());
    surf->SetGlobalAlpha((float)ctx->getProp("globalAlpha").toNumber());
}

} // namespace

JsValue MakeCanvasRenderingContext2D(VM& vm, Node* canvasNode) {
    auto* ctx = vm.gc().newObject(ObjKind::Plain);
    ctx->setProp("fillStyle", vm.str("#000000"));
    ctx->setProp("strokeStyle", vm.str("#000000"));
    ctx->setProp("lineWidth", JsValue::number(1.0));
    ctx->setProp("globalAlpha", JsValue::number(1.0));

    addCtxMethod(vm, ctx, "fillRect", [canvasNode](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode);
        if (surf && thisVal.isObject()) {
            SyncFillStyle(thisVal.asObject(), surf);
            surf->FillRect(NumArg(args, 0), NumArg(args, 1), NumArg(args, 2), NumArg(args, 3));
            MarkCanvasDirty(vm, canvasNode);
        }
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "strokeRect", [canvasNode](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue {
        ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode);
        if (surf && thisVal.isObject()) {
            SyncStrokeStyle(thisVal.asObject(), surf);
            surf->StrokeRect(NumArg(args, 0), NumArg(args, 1), NumArg(args, 2), NumArg(args, 3));
            MarkCanvasDirty(vm, canvasNode);
        }
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "clearRect", [canvasNode](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode)) {
            surf->ClearRect(NumArg(args, 0), NumArg(args, 1), NumArg(args, 2), NumArg(args, 3));
            MarkCanvasDirty(vm, canvasNode);
        }
        return JsValue::undefined();
    });

    addCtxMethod(vm, ctx, "beginPath", [canvasNode](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode)) surf->BeginPath();
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "closePath", [canvasNode](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode)) surf->ClosePath();
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "moveTo", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->MoveTo(NumArg(args, 0), NumArg(args, 1));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "lineTo", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->LineTo(NumArg(args, 0), NumArg(args, 1));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "rect", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->Rect(NumArg(args, 0), NumArg(args, 1), NumArg(args, 2), NumArg(args, 3));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "arc", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->Arc(NumArg(args, 0), NumArg(args, 1), NumArg(args, 2),
                      NumArg(args, 3), NumArg(args, 4), BoolArg(args, 5));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "bezierCurveTo", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->BezierCurveTo(NumArg(args, 0), NumArg(args, 1), NumArg(args, 2),
                                NumArg(args, 3), NumArg(args, 4), NumArg(args, 5));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "quadraticCurveTo", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->QuadraticCurveTo(NumArg(args, 0), NumArg(args, 1), NumArg(args, 2), NumArg(args, 3));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "fill", [canvasNode](VM& vm, JsValue thisVal, std::vector<JsValue>) -> JsValue {
        ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode);
        if (surf && thisVal.isObject()) {
            SyncFillStyle(thisVal.asObject(), surf);
            surf->Fill();
            MarkCanvasDirty(vm, canvasNode);
        }
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "stroke", [canvasNode](VM& vm, JsValue thisVal, std::vector<JsValue>) -> JsValue {
        ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode);
        if (surf && thisVal.isObject()) {
            SyncStrokeStyle(thisVal.asObject(), surf);
            surf->Stroke();
            MarkCanvasDirty(vm, canvasNode);
        }
        return JsValue::undefined();
    });

    addCtxMethod(vm, ctx, "save", [canvasNode](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode)) surf->Save();
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "restore", [canvasNode](VM&, JsValue, std::vector<JsValue>) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode)) surf->Restore();
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "translate", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->Translate(NumArg(args, 0), NumArg(args, 1));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "scale", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->Scale(NumArg(args, 0, 1.f), NumArg(args, 1, 1.f));
        return JsValue::undefined();
    });
    addCtxMethod(vm, ctx, "rotate", [canvasNode](VM&, JsValue, std::vector<JsValue> args) -> JsValue {
        if (ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode))
            surf->Rotate(NumArg(args, 0));
        return JsValue::undefined();
    });

    addCtxMethod(vm, ctx, "drawImage", [canvasNode](VM& vm, JsValue, std::vector<JsValue> args) -> JsValue {
        ICanvasSurface* surf = GetCanvasSurfaceForNode(canvasNode);
        if (!surf || args.empty() || !args[0].isObject()) return JsValue::undefined();
        Node* imgNode = unwrapNode(args[0]);
        if (!imgNode) return JsValue::undefined();
        std::string url = imgNode->attr("src");
        if (url.empty()) return JsValue::undefined();
        float dx = NumArg(args, 1), dy = NumArg(args, 2);
        // 0 here means "no explicit size given" — the surface resolves the
        // image's natural (intrinsic) size itself when dw/dh are <= 0,
        // matching the 3-argument drawImage(image, dx, dy) form.
        float dw = NumArg(args, 3, 0.f);
        float dh = NumArg(args, 4, 0.f);
        surf->DrawImage(url, dx, dy, dw, dh);
        MarkCanvasDirty(vm, canvasNode);
        return JsValue::undefined();
    });

    return JsValue::object(ctx);
}
