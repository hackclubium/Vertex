#pragma once
#include "js/vm.h"

struct Node;

// Builds the JS-facing CanvasRenderingContext2D object returned by
// canvas.getContext('2d'). Drawing methods forward to the ICanvasSurface
// registered for canvasNode via DomBridgeCallbacks::getCanvasSurface
// (js/dom_bridge.h), no-opping safely if none is wired up.
JsValue MakeCanvasRenderingContext2D(VM& vm, Node* canvasNode);
