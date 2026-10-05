// bands W H: BGRA glReadPixels of a W x H texture FBO into a strided buffer
// (as Summit's read-back does), whole and in bands of N rows.
#include <OS.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc, char** argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 1920, h = argc > 2 ? atoi(argv[2]) : 1000;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d, nullptr, nullptr); eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE };
    EGLConfig c; EGLint n; eglChooseConfig(d, ca, &c, 1, &n);
    const EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, c, pa);
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa); eglMakeCurrent(d, s, s, x);
    bool bgraTexture = argc > 3 && argv[3][0] == 'b';
    bool bgraRead = !(argc > 4 && argv[4][0] == 'r');
    GLuint tex, fbo; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    if (bgraTexture)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    std::printf("texture %s, read %s, status %x err %x\n", bgraTexture ? "BGRA" : "RGBA", bgraRead ? "BGRA" : "RGBA",
        0, glGetError());
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::printf("fbo status %x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
    int stride = w + 16;
    std::vector<unsigned char> out(size_t(stride) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, stride);
    const int heights[] = { 0, 64 };
    for (int band : heights) {
        double best = 1e9;
        for (int i = 0; i < 4; i++) {
            glClearColor(i * 0.1f, 0.5f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
            bigtime_t t0 = system_time();
            int step = band ? band : h;
            for (int y = 0; y < h; y += step) {
                int rows = y + step > h ? h - y : step;
                glPixelStorei(GL_PACK_SKIP_ROWS, y);
                glReadPixels(0, y, w, rows, bgraRead ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            }
            double ms = (system_time() - t0) / 1e3;
            if (ms < best) best = ms;
        }
        std::printf("band %3d rows: %6.1f ms  %4.0f MB/s  %5.1f ms/Mpx\n", band ? band : h, best, w * double(h) * 4 / 1e3 / best, best / (w * double(h) / 1e6));
    }
    return 0;
}
