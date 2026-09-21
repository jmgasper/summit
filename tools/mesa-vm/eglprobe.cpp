#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <Application.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <OS.h>

static int run()
{
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { printf("no display\n"); return 1; }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    printf("EGL %d.%d vendor=%s\n", major, minor, eglQueryString(display, EGL_VENDOR));
    printf("EGL client apis=%s\n", eglQueryString(display, EGL_CLIENT_APIS));
    printf("EGL extensions=%s\n", eglQueryString(display, EGL_EXTENSIONS));
    const char* clientExt = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL client extensions=%s\n", clientExt ? clientExt : "(none)");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) { printf("bind GLES failed 0x%x\n", eglGetError()); return 1; }
    EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config; EGLint count = 0;
    if (!eglChooseConfig(display, attrs, &config, 1, &count) || !count) {
        printf("no pbuffer config (0x%x); trying window-type config\n", eglGetError());
        EGLint attrs2[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        if (!eglChooseConfig(display, attrs2, &config, 1, &count) || !count) { printf("no config at all\n"); return 1; }
    }
    EGLint surfaceType = 0; eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surfaceType);
    printf("config surface type=0x%x (pbuffer=%d window=%d)\n", surfaceType, !!(surfaceType & EGL_PBUFFER_BIT), !!(surfaceType & EGL_WINDOW_BIT));
    EGLint contextAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    if (context == EGL_NO_CONTEXT) { printf("context failed 0x%x\n", eglGetError()); return 1; }
    EGLint pbAttrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbAttrs);
    printf("pbuffer surface=%p err=0x%x\n", surface, eglGetError());
    bool current = surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context);
    if (!current) {
        printf("pbuffer makeCurrent failed 0x%x; trying surfaceless\n", eglGetError());
        current = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
        if (!current) { printf("surfaceless failed 0x%x\n", eglGetError()); return 1; }
    }
    printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\nGLSL=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    printf("GL_EXTENSIONS=%s\n", glGetString(GL_EXTENSIONS));
    const int W = 1024, H = 768;
    GLuint texture = 0, framebuffer = 0;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    printf("fbo status=0x%x (complete=0x%x)\n", glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, W, H);
    std::vector<unsigned char> pixels(W * H * 4);
    bigtime_t start = system_time();
    const int frames = 60;
    for (int i = 0; i < frames; ++i) {
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    bigtime_t elapsed = system_time() - start;
    printf("pixel0=%u,%u,%u,%u glError=0x%x\n", pixels[0], pixels[1], pixels[2], pixels[3], glGetError());
    printf("clear+readback %dx%d: %.2f ms/frame\n", W, H, elapsed / 1000.0 / frames);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(display);
    return 0;
}
int main(int argc, char** argv)
{
    bool withApp = argc > 1 && !strcmp(argv[1], "--app");
    if (withApp) { BApplication app("application/x-vnd.summit-eglprobe"); return run(); }
    return run();
}
