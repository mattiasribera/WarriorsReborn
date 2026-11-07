/* VertexSink: the renderer's per-frame vertex array (src/renderer/vk_renderer.cpp). Header-only
 * so tests/vertex_sink_test.cpp checks it without a Vulkan device. */
#ifndef VERTEX_SINK_H
#define VERTEX_SINK_H
#include <cstring>
#include <vector>
#include "../renderer.h"

// The frame's vertices (renderer_draw, renderer_skyline, renderer_draw_skinned's reservations).
// Direct (RendererConfig.direct_geometry, after renderer_begin waited for the slot): written
// straight into the slot's mapped geometry buffer, where upload_geometry would have copied them,
// so the frame skips one copy of every vertex. A frame that outgrows the buffer moves the vertices
// written so far into the heap copy and goes on there (upload_geometry then grows the buffer and
// copies them as before). Not direct: the heap copy, uploaded at the end of the frame.
struct VertexSink {
    RVertex *map = nullptr;   // direct: the slot's mapped geometry buffer, cap vertices
    size_t cap = 0, n = 0;
    std::vector<RVertex> heap;   // not direct
    void reset(void *m, unsigned long long bytes) {
        heap.clear();
        n = 0;
        cap = m ? (size_t)(bytes / sizeof(RVertex)) : 0;
        map = cap ? (RVertex *)m : nullptr;
    }
    size_t size() const { return n; }
    bool direct() const { return map != nullptr; }
    const RVertex *data() const { return map ? map : heap.data(); }
    void leave_map() {   // reads the mapped vertices back once (a frame larger than the buffer)
        if (!map) return;
        heap.assign(map, map + n);
        map = nullptr;
    }
    // k vertices from v, or (v NULL) k vertices the compute pre-pass fills: left as they are in
    // the mapped buffer (its copy covers them before any pass reads them), zero in the heap copy.
    void add(const RVertex *v, size_t k) {
        if (map && n + k > cap) leave_map();
        if (map) {
            if (v) memcpy(map + n, v, k * sizeof *v);
        } else if (v) {
            heap.insert(heap.end(), v, v + k);
        } else {
            heap.resize(heap.size() + k);
        }
        n += k;
    }
};
#endif
