// readback W H: glReadPixels cost on the current GPU for an RGBA8 FBO:
// straight into client memory, and through a pixel pack buffer (PBO).
#include <OS.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 1920, h = argc > 2 ? atoi(argv[2]) : 1080;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d, nullptr, nullptr); eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE };
    EGLConfig c; EGLint n; eglChooseConfig(d, ca, &c, 1, &n);
    const EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, c, pa);
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa); eglMakeCurrent(d, s, s, x);
    GLuint tex, fbo; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::vector<unsigned char> out(size_t(w) * h * 4);
    double mb = w * double(h) * 4 / 1e6;
    for (int i = 0; i < 6; i++) {
        glClearColor(i * 0.1f, 0.5f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT);
        bigtime_t t0 = system_time(); glFinish(); bigtime_t t1 = system_time();
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        bigtime_t t2 = system_time();
        std::printf("client: finish %.1f ms read %.1f ms (%.0f MB/s) px=%02x\n", (t1 - t0) / 1e3, (t2 - t1) / 1e3, mb / ((t2 - t1) / 1e6), out[0]);
    }
    GLuint pbo; glGenBuffers(1, &pbo); glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, out.size(), nullptr, GL_STREAM_READ);
    for (int i = 0; i < 6; i++) {
        glClearColor(i * 0.1f, 0.3f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT);
        bigtime_t t0 = system_time();
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        bigtime_t t1 = system_time();
        void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, out.size(), GL_MAP_READ_BIT);
        bigtime_t t2 = system_time();
        if (p) memcpy(out.data(), p, out.size());
        bigtime_t t3 = system_time();
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        std::printf("pbo: readpixels %.1f ms map %.1f ms copy %.1f ms (%.0f MB/s) px=%02x\n", (t1 - t0) / 1e3, (t2 - t1) / 1e3,
            (t3 - t2) / 1e3, mb / ((t3 - t2) / 1e6), out[0]);
    }
    // The pbuffer's own colour buffer (window-system surface) instead of a texture FBO.
    const EGLint pb[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface big = eglCreatePbufferSurface(d, c, pb);
    eglMakeCurrent(d, big, big, x);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int i = 0; i < 6; i++) {
        glClearColor(i * 0.1f, 0.4f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT);
        bigtime_t t0 = system_time(); glFinish(); bigtime_t t1 = system_time();
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        bigtime_t t2 = system_time();
        std::printf("pbuffer: finish %.1f ms read %.1f ms (%.0f MB/s) px=%02x\n", (t1 - t0) / 1e3, (t2 - t1) / 1e3, mb / ((t2 - t1) / 1e6), out[0]);
    }
    // A 64-row band of the texture FBO, as a damage-limited read does.
    eglMakeCurrent(d, s, s, x); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (int i = 0; i < 3; i++) {
        glClear(GL_COLOR_BUFFER_BIT); glFinish();
        bigtime_t t1 = system_time();
        glReadPixels(0, 64, w, 64, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        bigtime_t t2 = system_time();
        std::printf("band 64 rows: %.2f ms (%.0f MB/s)\n", (t2 - t1) / 1e3, (w * 64 * 4 / 1e6) / ((t2 - t1) / 1e6));
    }
    std::printf("%s\n", (const char*)glGetString(GL_RENDERER));
    return 0;
}
