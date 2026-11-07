// Game-independent test of the renderer's per-frame vertex array (src/renderer/vertex_sink.h):
// direct writes into a mapped buffer, the move to the heap copy when a frame outgrows it, and the
// heap-only path all yield the same vertices as one plain array.
#include "../src/renderer/vertex_sink.h"
#include <cstdio>
#include <cstdlib>

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) { printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } \
    } while (0)

static RVertex vert(int i) {
    RVertex v;
    unsigned char *b = (unsigned char *)&v;
    for (size_t k = 0; k < sizeof v; k++) b[k] = (unsigned char)(i * 31 + k * 7 + 1);
    return v;
}

// Appends the same sequence to the sink and to a reference array: k vertices from i0, or k
// reserved vertices when i0 < 0 (zero in the reference).
static std::vector<bool> unspecified;
static void add(VertexSink &s, std::vector<RVertex> &ref, int i0, size_t k) {
    if (ref.empty()) unspecified.clear();
    std::vector<RVertex> v;
    for (size_t j = 0; j < k; j++) v.push_back(i0 < 0 ? RVertex{} : vert(i0 + (int)j));
    if (i0 < 0) {
        RVertex z;
        memset(&z, 0, sizeof z);
        for (size_t j = 0; j < k; j++) ref.push_back(z);
        s.add(nullptr, k);
        unspecified.insert(unspecified.end(), k, s.direct());
    } else {
        ref.insert(ref.end(), v.begin(), v.end());
        s.add(v.data(), k);
        unspecified.insert(unspecified.end(), k, false);
    }
}

// Equal to the reference, except vertices reserved (v NULL) while the sink wrote to the mapped
// memory: unspecified (the pre-pass overwrites them), also after they moved to the heap copy.
static bool same(const VertexSink &s, const std::vector<RVertex> &ref) {
    if (s.size() != ref.size()) return false;
    for (size_t i = 0; i < ref.size(); i++)
        if (!unspecified[i] && memcmp(&s.data()[i], &ref[i], sizeof(RVertex))) return false;
    return true;
}

int main() {
    std::vector<unsigned char> mem(10 * sizeof(RVertex) + 5, 0xAB);   // 10 whole vertices
    VertexSink s;
    std::vector<RVertex> ref;

    // Direct, fits: the vertices land in the mapped memory, in order.
    s.reset(mem.data(), mem.size());
    CHECK(s.direct() && s.cap == 10 && s.size() == 0);
    add(s, ref, 1, 4);
    add(s, ref, -1, 3);   // reserved (zero) vertices
    add(s, ref, 50, 3);   // exactly full
    CHECK(s.direct());
    CHECK(s.data() == (const RVertex *)mem.data());
    CHECK(same(s, ref));
    CHECK(mem[10 * sizeof(RVertex)] == 0xAB);   // nothing past the whole vertices

    // Outgrowing the buffer: the vertices so far move to the heap copy, the rest follow there.
    add(s, ref, 100, 2);
    CHECK(!s.direct());
    CHECK(same(s, ref));
    add(s, ref, -1, 5);
    add(s, ref, 200, 7);
    CHECK(same(s, ref));

    // Reserved vertices leave the mapped memory as it was (no zeroing of what the pre-pass writes).
    ref.clear();
    memset(mem.data(), 0xCD, mem.size());
    s.reset(mem.data(), mem.size());
    add(s, ref, 1, 2);
    add(s, ref, -1, 3);
    CHECK(s.direct() && mem[2 * sizeof(RVertex)] == 0xCD && mem[5 * sizeof(RVertex) - 1] == 0xCD);
    CHECK(same(s, ref));

    // A new frame starts empty; leave_map by the uploader keeps the content.
    ref.clear();
    s.reset(mem.data(), mem.size());
    add(s, ref, 300, 6);
    s.leave_map();
    CHECK(!s.direct() && same(s, ref));
    s.leave_map();   // twice is harmless
    CHECK(same(s, ref));

    // Not direct: no buffer, or one smaller than a vertex.
    for (unsigned long long bytes : {0ull, (unsigned long long)sizeof(RVertex) - 1}) {
        ref.clear();
        s.reset(bytes ? mem.data() : nullptr, bytes);
        CHECK(!s.direct() && s.size() == 0);
        add(s, ref, 400, 3);
        add(s, ref, -1, 2);
        CHECK(!s.direct() && same(s, ref));
    }

    // Many draws of random sizes against a small buffer: always the reference.
    srand(7);
    for (int round = 0; round < 200; round++) {
        ref.clear();
        s.reset(mem.data(), mem.size());
        int draws = rand() % 8;
        for (int d = 0; d < draws; d++) add(s, ref, rand() % 2 ? -1 : rand() % 1000, (size_t)(rand() % 6));
        CHECK(same(s, ref));
        CHECK(s.direct() == (ref.size() <= 10));
    }

    if (fails) { printf("vertex_sink_test: %d FAILED\n", fails); return 1; }
    printf("vertex_sink_test: ALL OK\n");
    return 0;
}
