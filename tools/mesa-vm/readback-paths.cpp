// Which framebuffer format and read format make glReadPixels cheapest on
// zink, and what does a frame cost when something was drawn first?
// build: g++ -O2 -o readback-paths readback-paths.cpp -I$PREFIX/include -L$PREFIX/lib -lEGL -lGLESv2 -lbe
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif

static int W, H;

static void run(const char* name, GLenum readFormat, int redByte, int rounds, uint8_t* dst)
{
    bigtime_t total = 0, worst = 0, finish = 0;
    int wrong = 0;
    for (int i = 0; i < rounds + 3; ++i) {
        uint8_t want = uint8_t((i * 37 + 11) & 0xff);
        glClearColor(want / 255.0f, 0.5f, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        bigtime_t t0 = system_time();
        glFinish();
        bigtime_t start = system_time();
        glReadPixels(0, 0, W, H, readFormat, GL_UNSIGNED_BYTE, dst);
        bigtime_t elapsed = system_time() - start;
        if (i < 3) continue; // warm-up
        finish += start - t0;
        total += elapsed;
        if (elapsed > worst) worst = elapsed;
        size_t last = size_t(W) * H * 4 - 4;
        if (abs(int(dst[redByte]) - want) > 1 || abs(int(dst[last + redByte]) - want) > 1) ++wrong;
    }
    printf("%-34s finish %5.2f ms  read mean %6.2f ms  worst %6.2f ms  wrong %d/%d  error 0x%x\n", name, finish / 1000.0 / rounds, total / 1000.0 / rounds, worst / 1000.0, wrong, rounds, glGetError());
}

int main(int argc, char** argv)
{
    W = argc > 1 ? atoi(argv[1]) : 3840;
    H = argc > 2 ? atoi(argv[2]) : 1756;
    int rounds = argc > 3 ? atoi(argv[3]) : 30;
    size_t length = size_t(W) * H * 4;
    // Page-aligned like the shared bitmap the browser reads into.
    void* address = nullptr;
    area_id area = create_area("readback target", &address, B_ANY_ADDRESS, (length + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1), B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
    if (area < 0) return 1;
    auto* dst = static_cast<uint8_t*>(address);
    memset(dst, 0, length);

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) return 1;
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
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    printf("EXT_texture_format_BGRA8888 %d  EXT_read_format_bgra %d  EXT_buffer_storage %d\n", !!strstr(extensions, "GL_EXT_texture_format_BGRA8888"),
        !!strstr(extensions, "GL_EXT_read_format_bgra"), !!strstr(extensions, "GL_EXT_buffer_storage"));

    bigtime_t best = 0;
    std::vector<uint8_t> other(length, 3);
    for (int i = 0; i < 5; ++i) { bigtime_t s = system_time(); memcpy(dst, other.data(), length); bigtime_t t = system_time() - s; if (!best || t < best) best = t; }
    printf("memcpy of a frame: %.2f ms\n", best / 1000.0);

    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    GLuint rb;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, W, H);
    run("RGBA8 renderbuffer, read BGRA", GL_BGRA_EXT, 2, rounds, dst);
    run("RGBA8 renderbuffer, read RGBA", GL_RGBA, 0, rounds, dst);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, W, H, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
    printf("BGRA texture: error 0x%x\n", glGetError());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    printf("BGRA texture framebuffer status 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        run("BGRA texture, read BGRA", GL_BGRA_EXT, 2, rounds, dst);
        run("BGRA texture, read RGBA", GL_RGBA, 0, rounds, dst);
    }
    GLuint rb2;
    glGenRenderbuffers(1, &rb2);
    glBindRenderbuffer(GL_RENDERBUFFER, rb2);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_BGRA8_EXT, W, H);
    printf("BGRA8 renderbuffer: error 0x%x\n", glGetError());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb2);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        run("BGRA8 renderbuffer, read BGRA", GL_BGRA_EXT, 2, rounds, dst);
    return 0;
}
