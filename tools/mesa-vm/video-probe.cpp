// What a composited video frame costs on the GPU: upload a 1920x1080 frame,
// draw it scaled into a 3840x1756 target, read the part that changed back.
// build: g++ -O2 -o video-probe video-probe.cpp -I$PREFIX/include -L$PREFIX/lib -lEGL -lGLESv2 -lbe
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static GLuint compile(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); printf("shader: %s\n", log); }
    return shader;
}

int main(int argc, char** argv)
{
    int W = 3840, H = 1756, VW = argc > 1 ? atoi(argv[1]) : 1920, VH = argc > 2 ? atoi(argv[2]) : 1080;
    int DX = 32, DY = 136, DW = 2664, DH = 1498;
    int rounds = argc > 3 ? atoi(argv[3]) : 60;
    bool linearFilter = argc > 4 ? atoi(argv[4]) : 1;
    bool bgraTexture = argc > 5 ? atoi(argv[5]) : 1;
    int pace = argc > 6 ? atoi(argv[6]) : 16667;
    bool whole = argc > 7 ? atoi(argv[7]) : 0;
    if (whole) { DX = 0; DY = 0; DW = W; DH = H; }
    if (argc > 11) { DX = atoi(argv[8]); DY = atoi(argv[9]); DW = atoi(argv[10]); DH = atoi(argv[11]); }
    // Layers drawn over the frame, as a page's overlays are: blended (1) or opaque (2) quads of the same size.
    int layers = argc > 12 ? atoi(argv[12]) : 0;
    int layerMode = argc > 13 ? atoi(argv[13]) : 1;
    bool skipRead = argc > 14 ? atoi(argv[14]) : 0;
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

    GLuint fb, rb, ds;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, 0x93A1, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glGenRenderbuffers(1, &ds);
    glBindRenderbuffer(GL_RENDERBUFFER, ds);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
    printf("framebuffer 0x%x, video %dx%d %s, %s filter\n", glCheckFramebufferStatus(GL_FRAMEBUFFER), VW, VH, bgraTexture ? "BGRA" : "RGBA", linearFilter ? "linear" : "nearest");
    glViewport(0, 0, W, H);

    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, "attribute vec2 p; varying vec2 t; uniform vec4 r; void main() { t = p; gl_Position = vec4(r.xy + p * r.zw, 0.0, 1.0); }"));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 t; uniform sampler2D s; void main() { gl_FragColor = texture2D(s, t); }"));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    glUseProgram(program);
    // Destination rectangle in clip space.
    glUniform4f(glGetUniformLocation(program, "r"), -1 + 2.0f * DX / W, -1 + 2.0f * DY / H, 2.0f * DW / W, 2.0f * DH / H);
    static const GLfloat quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);

    GLenum format = bgraTexture ? GL_BGRA_EXT : GL_RGBA;
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linearFilter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linearFilter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, format, VW, VH, 0, format, GL_UNSIGNED_BYTE, nullptr);

    std::vector<uint8_t> frame(size_t(VW) * VH * 4);
    void* address = nullptr;
    size_t length = size_t(W) * H * 4;
    create_area("target", &address, B_ANY_ADDRESS, (length + 4095) & ~size_t(4095), B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
    memset(address, 0, length);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    bigtime_t upload = 0, draw = 0, finish = 0, read = 0;
    for (int i = 0; i < rounds + 5; ++i) {
        memset(frame.data(), i * 3, frame.size() / 64);
        bigtime_t t0 = system_time();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VW, VH, format, GL_UNSIGNED_BYTE, frame.data());
        bigtime_t t1 = system_time();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (layers) {
            if (layerMode == 1) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); }
            for (int layer = 0; layer < layers; ++layer)
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);
        }
        glFlush();
        bigtime_t t2 = system_time();
        glFinish();
        bigtime_t t3 = system_time();
        glPixelStorei(GL_PACK_ROW_LENGTH, W);
        glPixelStorei(GL_PACK_SKIP_PIXELS, DX);
        glPixelStorei(GL_PACK_SKIP_ROWS, DY);
        if (!skipRead)
            glReadPixels(DX, DY, DW, DH, GL_BGRA_EXT, GL_UNSIGNED_BYTE, address);
        bigtime_t t4 = system_time();
        if (i >= 5) { upload += t1 - t0; draw += t2 - t1; finish += t3 - t2; read += t4 - t3; }
        bigtime_t spent = t4 - t0;
        if (pace && spent < pace) snooze(pace - spent);
    }
    printf("layers %d mode %d, rect %d,%d %dx%d: upload %.2f ms, draw+flush %.2f ms, finish %.2f ms, read %.2f Mpx %.2f ms; error 0x%x\n", layers, layerMode, DX, DY, DW, DH, upload / 1000.0 / rounds, draw / 1000.0 / rounds,
        finish / 1000.0 / rounds, DW * double(DH) / 1e6, read / 1000.0 / rounds, glGetError());
    return 0;
}
