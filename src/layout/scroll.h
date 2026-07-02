#pragma once

#include <algorithm>

inline float FragmentReachableDocumentHeight(float documentHeight, float anchorY, float viewportHeight) {
    return std::max(documentHeight, anchorY + viewportHeight);
}
