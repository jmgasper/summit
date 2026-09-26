// How fast can a frame come back from the GPU? Times each way Summit's web
// process could read a composited frame into shared memory, at a given size:
//   direct  glReadPixels into client memory (what Skia's readPixels does)
//   pbo     glReadPixels into a pixel buffer object, then map and copy, with
//           the GPU copy, the wait for it and the CPU copy timed separately
//   memcpy  the same copy between two ordinary buffers, as the floor
// for RGBA and, when EXT_read_format_bgra is there, BGRA.
//
// build: g++ -O2 -o readback-probe readback-probe.cpp \
//            -I$PREFIX/include -L$PREFIX/lib -lEGL -lGLESv2 -lbe
// run:   LIBRARY_PATH=$PREFIX/lib:/boot/system/lib ./readback-probe 3840 1826
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <OS.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static double ms(bigtime_t t) { return t / 1000.0; }

int main(int argc, char** argv)
{
    int width = argc > 1 ? atoi(argv[1]) : 3840;
    int height = argc > 2 ? atoi(argv[2]) : 1826;
    int rounds = argc > 3 ? atoi(argv[3]) : 20;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) {
        printf("eglInitialize failed\n");
        return 1;
    }
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
    if (!eglMakeCurrent(display, surface, surface, context)) {
        printf("eglMakeCurrent failed\n");
        return 1;
    }
    printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    bool bgra = extensions && strstr(extensions, "GL_EXT_read_format_bgra");
    printf("EXT_read_format_bgra=%d size=%dx%d (%.2f Mpx, %.1f MB)\n", bgra, width, height,
        width * double(height) / 1e6, width * double(height) * 4 / 1e6);

    GLuint renderbuffer, framebuffer;
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("framebuffer incomplete\n");
        return 1;
    }

    size_t length = size_t(width) * height * 4;
    std::vector<uint8_t> destination(length), source(length, 7);
    GLenum formats[] = { GL_RGBA, GL_BGRA_EXT };
    const char* names[] = { "RGBA", "BGRA" };
    for (int f = 0; f < (bgra ? 2 : 1); ++f) {
        // direct
        bigtime_t total = 0, worst = 0;
        for (int i = 0; i < rounds; ++i) {
            glClearColor(i / float(rounds), 0.5f, 0.25f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glFinish();
            bigtime_t start = system_time();
            glReadPixels(0, 0, width, height, formats[f], GL_UNSIGNED_BYTE, destination.data());
            bigtime_t elapsed = system_time() - start;
            total += elapsed;
            if (elapsed > worst)
                worst = elapsed;
        }
        printf("direct %s: mean %.2f ms, worst %.2f ms, error 0x%x\n", names[f], ms(total / rounds), ms(worst), glGetError());

        for (GLenum usage : { GL_STREAM_READ, GL_DYNAMIC_READ, GL_STATIC_READ }) {
            GLuint buffer;
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
            glBufferData(GL_PIXEL_PACK_BUFFER, length, nullptr, usage);
            bigtime_t read = 0, map = 0, copy = 0, worstAll = 0;
            for (int i = 0; i < rounds; ++i) {
                glClearColor(i / float(rounds), 0.5f, 0.25f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glFinish();
                bigtime_t start = system_time();
                glReadPixels(0, 0, width, height, formats[f], GL_UNSIGNED_BYTE, nullptr);
                bigtime_t issued = system_time();
                auto* pixels = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, length, GL_MAP_READ_BIT));
                bigtime_t mapped = system_time();
                if (pixels)
                    memcpy(destination.data(), pixels, length);
                bigtime_t copied = system_time();
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                read += issued - start;
                map += mapped - issued;
                copy += copied - mapped;
                if (copied - start > worstAll)
                    worstAll = copied - start;
            }
            printf("pbo %s usage 0x%x: read %.2f + map %.2f + copy %.2f ms, worst %.2f ms, error 0x%x\n", names[f], usage,
                ms(read / rounds), ms(map / rounds), ms(copy / rounds), ms(worstAll), glGetError());
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glDeleteBuffers(1, &buffer);
        }
    }
    bigtime_t start = system_time();
    for (int i = 0; i < rounds; ++i)
        memcpy(destination.data(), source.data(), length);
    printf("memcpy: %.2f ms\n", ms((system_time() - start) / rounds));
    return 0;
}
