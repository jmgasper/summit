// Build like readback-probe.cpp; run: readback-verify [width height rounds]
// Which pixel-buffer readbacks return the right frame on zink/NVK, and how fast?
// Each round clears to a new colour, reads back, and checks the pixels.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

static int W, H;
static size_t LEN;
static std::vector<uint8_t> dst;

static uint8_t expectedRed(int round) { return uint8_t((round * 37 + 11) & 0xff); }

static void draw(int round)
{
    glClearColor(expectedRed(round) / 255.0f, 0.5f, 0.25f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

// BGRA: red is byte 2.
static bool check(int round)
{
    uint8_t want = expectedRed(round);
    for (size_t at : { size_t(0), LEN / 2 & ~size_t(3), LEN - 4 }) {
        if (std::abs(int(dst[at + 2]) - int(want)) > 1)
            return false;
    }
    return true;
}

static void run(const char* name, int rounds, const std::function<bool(int)>& readRound)
{
    bigtime_t total = 0, worst = 0;
    int wrong = 0;
    for (int i = 0; i < rounds; ++i) {
        draw(i);
        bigtime_t start = system_time();
        bool ok = readRound(i);
        bigtime_t elapsed = system_time() - start;
        total += elapsed;
        if (elapsed > worst)
            worst = elapsed;
        if (!ok)
            ++wrong;
    }
    printf("%-26s mean %6.2f ms  worst %6.2f ms  wrong %d/%d  error 0x%x\n", name, total / 1000.0 / rounds, worst / 1000.0, wrong, rounds, glGetError());
}

int main(int argc, char** argv)
{
    W = argc > 1 ? atoi(argv[1]) : 3840;
    H = argc > 2 ? atoi(argv[2]) : 1826;
    int rounds = argc > 3 ? atoi(argv[3]) : 30;
    LEN = size_t(W) * H * 4;
    dst.resize(LEN);

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor))
        return 1;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config;
    EGLint count = 0;
    eglChooseConfig(display, configAttributes, &config, 1, &count);
    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    const EGLint surfaceAttributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    eglMakeCurrent(display, surface, surface, context);
    printf("GL_RENDERER=%s %dx%d\n", glGetString(GL_RENDERER), W, H);

    GLuint rb, fb;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

    run("direct", rounds, [](int i) {
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, dst.data());
        return check(i);
    });

    auto mapCopy = [](GLuint buffer) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
        auto* p = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, LEN, GL_MAP_READ_BIT));
        if (p)
            memcpy(dst.data(), p, LEN);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        return p != nullptr;
    };

    GLuint reused;
    glGenBuffers(1, &reused);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, reused);
    glBufferData(GL_PIXEL_PACK_BUFFER, LEN, nullptr, GL_STREAM_READ);
    run("pbo reused", rounds, [&](int i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, reused);
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        return mapCopy(reused) && check(i);
    });
    run("pbo reused + fence", rounds, [&](int i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, reused);
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
        glDeleteSync(sync);
        return mapCopy(reused) && check(i);
    });
    run("pbo orphaned", rounds, [&](int i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, reused);
        glBufferData(GL_PIXEL_PACK_BUFFER, LEN, nullptr, GL_STREAM_READ);
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        return mapCopy(reused) && check(i);
    });
    run("pbo fresh", rounds, [&](int i) {
        GLuint b;
        glGenBuffers(1, &b);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
        glBufferData(GL_PIXEL_PACK_BUFFER, LEN, nullptr, GL_STREAM_READ);
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        bool ok = mapCopy(b) && check(i);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glDeleteBuffers(1, &b);
        return ok;
    });
    // Ring: read this frame into one buffer, map the one read a frame ago.
    GLuint ring[3];
    glGenBuffers(3, ring);
    for (GLuint b : ring) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
        glBufferData(GL_PIXEL_PACK_BUFFER, LEN, nullptr, GL_STREAM_READ);
    }
    run("pbo ring (1 frame late)", rounds, [&](int i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ring[i % 3]);
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        glFlush();
        if (!i)
            return true;
        return mapCopy(ring[(i - 1) % 3]) && check(i - 1);
    });
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    // A partial read: the 1280x720 middle, as damage would ask.
    int pw = std::min(W, 2560), ph = std::min(H, 1440);
    run("direct partial 2560x1440", rounds, [&](int i) {
        glPixelStorei(GL_PACK_ROW_LENGTH, W);
        glReadPixels(0, 0, pw, ph, GL_BGRA_EXT, GL_UNSIGNED_BYTE, dst.data());
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        return std::abs(int(dst[2]) - int(expectedRed(i))) <= 1;
    });

    // What does a mapped buffer hold: this frame, an older one, or zeros?
    GLuint probe;
    glGenBuffers(1, &probe);
    for (int i = 0; i < 6; ++i) {
        draw(100 + i);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, probe);
        glBufferData(GL_PIXEL_PACK_BUFFER, LEN, nullptr, GL_STREAM_READ);
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        glFinish();
        for (int wait : { 0, 50, 500 }) {
            if (wait)
                snooze(wait * 1000);
            auto* p = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, LEN, GL_MAP_READ_BIT));
            printf("round %d wait %3d ms: want red %3d, first pixel BGRA %3d %3d %3d %3d, middle red %3d\n", i, wait, expectedRed(100 + i),
                p ? p[0] : -1, p ? p[1] : -1, p ? p[2] : -1, p ? p[3] : -1, p ? p[(LEN / 2 & ~size_t(3)) + 2] : -1);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return 0;
}
