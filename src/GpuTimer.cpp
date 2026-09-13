#include "GpuTimer.h"

#include "GlInterop.h"
#include "Log.h"

#include <cstdio>
#include <cstring>

namespace gplvr {
namespace {

// ARB_timer_query / GL 3.3. Not in the GL 1.1 header the Windows SDK ships.
constexpr GLenum kGlTimestamp = 0x8E28;
constexpr GLenum kGlQueryResult = 0x8866;
constexpr GLenum kGlQueryResultAvailable = 0x8867;

typedef void(WINAPI* GlGenQueriesFn)(GLsizei, GLuint*);
typedef void(WINAPI* GlQueryCounterFn)(GLuint, GLenum);
typedef void(WINAPI* GlGetQueryObjectivFn)(GLuint, GLenum, GLint*);
typedef void(WINAPI* GlGetQueryObjecti64vFn)(GLuint, GLenum, long long*);
typedef void(WINAPI* GlGetInteger64vFn)(GLenum, long long*);

// Eight frames of records: the pipeline is rarely more than three deep, and a GPU so far
// behind that all eight are still pending is reported as skipped frames rather than waited
// for.
constexpr int kRecords = 8;
constexpr int kMarks = 32;

struct Record {
    GLuint    query[kMarks] = {};
    int       group[kMarks] = {};
    int       marks = 0;
    bool      pending = false;
    long long submittedEnd = 0;  // GL time when the closing mark was issued
    unsigned  closedAt = 0;
};

struct State {
    GlFunctions gl;
    bool        attempted = false;
    bool        available = false;
    HGLRC       context = nullptr;

    GlGenQueriesFn         genQueries = nullptr;
    GlQueryCounterFn       queryCounter = nullptr;
    GlGetQueryObjectivFn   getQueryObjectiv = nullptr;
    GlGetQueryObjecti64vFn getQueryObjecti64v = nullptr;
    GlGetInteger64vFn      getInteger64v = nullptr;

    Record   records[kRecords];
    int      open = -1;
    unsigned frame = 0;

    long long          groupNs[kGpuTimerMaxGroups] = {};
    long long          frameNs = 0;
    long long          queueNs = 0;
    long long          queueWorstNs = 0;
    unsigned long long lateFrames = 0;
    unsigned           framesRead = 0;
    unsigned           skipped = 0;
};

State g_gpu;

template <typename T>
bool resolveProc(State& s, const char* name, T& out) {
    out = reinterpret_cast<T>(s.gl.wglGetProcAddress(name));
    if (!out) logf("gpu: %s did not resolve", name);
    return out != nullptr;
}

// Once, with a context current. roglv2's context is a compatibility one, so the extension
// string is only asked for when the version alone does not settle it.
bool resolve() {
    State& s = g_gpu;
    if (s.attempted) return s.available;
    if (!glResolve(s.gl) || !s.gl.wglGetCurrentContext()) return false;
    s.attempted = true;

    auto version = reinterpret_cast<const char*>(s.gl.getString(GL_VERSION));
    int  major = 0;
    int  minor = 0;
    if (version) sscanf_s(version, "%d.%d", &major, &minor);
    bool core = major > 3 || (major == 3 && minor >= 3);
    bool extension = false;
    if (!core) {
        auto extensions = reinterpret_cast<const char*>(s.gl.getString(GL_EXTENSIONS));
        extension = extensions && std::strstr(extensions, "GL_ARB_timer_query");
    }
    if (!core && !extension) {
        logf("gpu: GL %s has no ARB_timer_query, so GPU span times are not measured",
             version ? version : "(version unknown)");
        return false;
    }

    bool complete = resolveProc(s, "glGenQueries", s.genQueries);
    complete = resolveProc(s, "glQueryCounter", s.queryCounter) && complete;
    complete = resolveProc(s, "glGetQueryObjectiv", s.getQueryObjectiv) && complete;
    complete = resolveProc(s, "glGetQueryObjecti64v", s.getQueryObjecti64v) && complete;
    complete = resolveProc(s, "glGetInteger64v", s.getInteger64v) && complete;
    if (!complete) {
        logf("gpu: timer queries are advertised but their entry points are incomplete -- "
             "GPU span times are not measured");
        return false;
    }

    s.available = true;
    logf("gpu: GL_TIMESTAMP timer queries on (GL %s) -- GPU span times are read back a "
         "few frames late, never waited for",
         version);
    return true;
}

// Query names belong to the context that made them. roglv2 builds a new context on every
// menu-to-sim transition, and the old names are gone with it, so they are dropped rather
// than deleted.
void adoptContext(HGLRC context) {
    State& s = g_gpu;
    if (context == s.context) return;
    s.context = context;
    s.open = -1;
    for (Record& record : s.records) {
        record.marks = 0;
        record.pending = false;
        s.genQueries(kMarks, record.query);
    }
}

void readBack() {
    State& s = g_gpu;
    for (Record& record : s.records) {
        if (!record.pending) continue;

        GLint ready = 0;
        s.getQueryObjectiv(record.query[record.marks - 1], kGlQueryResultAvailable, &ready);
        if (!ready) continue;

        long long stamp[kMarks] = {};
        bool      complete = true;
        for (int m = 0; m < record.marks && complete; ++m) {
            ready = 0;
            s.getQueryObjectiv(record.query[m], kGlQueryResultAvailable, &ready);
            if (ready)
                s.getQueryObjecti64v(record.query[m], kGlQueryResult, &stamp[m]);
            else
                complete = false;
        }
        if (!complete) continue;
        record.pending = false;

        for (int m = 0; m + 1 < record.marks; ++m) {
            long long spent = stamp[m + 1] - stamp[m];
            int       group = record.group[m];
            if (spent > 0 && group >= 0 && group < kGpuTimerMaxGroups)
                s.groupNs[group] += spent;
        }
        long long frame = stamp[record.marks - 1] - stamp[0];
        if (frame > 0) s.frameNs += frame;

        long long queue = stamp[record.marks - 1] - record.submittedEnd;
        if (queue < 0) queue = 0;
        s.queueNs += queue;
        if (queue > s.queueWorstNs) s.queueWorstNs = queue;

        s.lateFrames += s.frame - record.closedAt;
        ++s.framesRead;
    }
}

}  // namespace

void gpuTimerMark(int group) {
    State& s = g_gpu;
    if (s.open < 0) return;
    Record& record = s.records[s.open];
    // The last slot belongs to the frame's closing mark.
    if (record.marks >= kMarks - 1) return;
    if (s.gl.wglGetCurrentContext() != s.context) {
        s.open = -1;
        return;
    }
    s.queryCounter(record.query[record.marks], kGlTimestamp);
    record.group[record.marks] = group;
    ++record.marks;
}

void gpuTimerEndFrame(int firstGroup) {
    State& s = g_gpu;
    if (!resolve()) return;

    HGLRC context = s.gl.wglGetCurrentContext();
    if (!context) {
        s.open = -1;
        return;
    }
    adoptContext(context);
    ++s.frame;

    if (s.open >= 0) {
        Record& record = s.records[s.open];
        if (record.marks > 0) {
            s.queryCounter(record.query[record.marks], kGlTimestamp);
            record.group[record.marks] = -1;
            ++record.marks;
            // The GL time at which the closing mark was issued, on the same clock as the
            // query results -- so their difference is how far behind the CPU the GPU is.
            s.getInteger64v(kGlTimestamp, &record.submittedEnd);
            record.closedAt = s.frame;
            record.pending = record.marks >= 2;
        }
        s.open = -1;
    }

    readBack();

    for (int i = 0; i < kRecords; ++i) {
        if (s.records[i].pending) continue;
        s.open = i;
        break;
    }
    if (s.open < 0) {
        ++s.skipped;
        return;
    }
    s.records[s.open].marks = 0;
    gpuTimerMark(firstGroup);
}

bool gpuTimerTake(GpuTimerWindow& out) {
    State& s = g_gpu;
    out = GpuTimerWindow();
    bool measured = s.available && (s.framesRead || s.skipped);

    if (s.framesRead) {
        double perFrame = 1e-6 / s.framesRead;
        for (int i = 0; i < kGpuTimerMaxGroups; ++i) out.groupMs[i] = s.groupNs[i] * perFrame;
        out.frameMs = s.frameNs * perFrame;
        out.queueMs = s.queueNs * perFrame;
        out.queueWorstMs = s.queueWorstNs * 1e-6;
        out.framesLate = static_cast<double>(s.lateFrames) / s.framesRead;
    }
    out.frames = s.framesRead;
    out.skipped = s.skipped;

    for (long long& group : s.groupNs) group = 0;
    s.frameNs = 0;
    s.queueNs = 0;
    s.queueWorstNs = 0;
    s.lateFrames = 0;
    s.framesRead = 0;
    s.skipped = 0;
    return measured;
}

}  // namespace gplvr
