// Does the GPU time of a composition follow its pixels or its draw calls?
// Draws a 3840x1756 target covered by tiles, each its own texture and its own
// draw call with its own uniforms, as TextureMapper draws a layer.
//   tiles-probe [tileSize layers rounds uploadPerFrame]
// build: g++ -O2 -o tiles-probe tiles-probe.cpp -I$PREFIX/include -L$PREFIX/lib -lEGL -lGLESv2 -lbe
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
    return shader;
}

int main(int argc, char** argv)
{
    const int W = 3840, H = 1756;
    int tile = argc > 1 ? atoi(argv[1]) : 256;
    int layers = argc > 2 ? atoi(argv[2]) : 1;
    int rounds = argc > 3 ? atoi(argv[3]) : 60;
    int uploads = argc > 4 ? atoi(argv[4]) : 0;
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
    glViewport(0, 0, W, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, W, H);

    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, "attribute vec2 p; varying vec2 t; uniform vec4 r; void main() { t = p; gl_Position = vec4(r.xy + p * r.zw, 0.0, 1.0); }"));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 t; uniform sampler2D s; uniform float o; void main() { gl_FragColor = texture2D(s, t) * o; }"));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    glUseProgram(program);
    GLint rect = glGetUniformLocation(program, "r");
    GLint opacity = glGetUniformLocation(program, "o");
    static const GLfloat quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);

    int columns = (W + tile - 1) / tile, rows = (H + tile - 1) / tile;
    int tiles = columns * rows;
    std::vector<uint8_t> pixels(size_t(tile) * tile * 4, 0x80);
    std::vector<GLuint> textures(size_t(tiles) * layers);
    glGenTextures(textures.size(), textures.data());
    for (GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, tile, tile, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());
    }
    glFinish();

    bigtime_t draw = 0, finish = 0, upload = 0;
    for (int round = 0; round < rounds + 5; ++round) {
        bigtime_t t0 = system_time();
        for (int i = 0; i < uploads; ++i) {
            glBindTexture(GL_TEXTURE_2D, textures[(round * uploads + i) % textures.size()]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tile, tile, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());
        }
        bigtime_t t1 = system_time();
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (int layer = 0; layer < layers; ++layer) {
            if (layer) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); } else glDisable(GL_BLEND);
            for (int i = 0; i < tiles; ++i) {
                int x = (i % columns) * tile, y = (i / columns) * tile - (round * 7 % tile);
                glBindTexture(GL_TEXTURE_2D, textures[size_t(layer) * tiles + i]);
                glUniform4f(rect, -1 + 2.0f * x / W, -1 + 2.0f * y / H, 2.0f * tile / W, 2.0f * tile / H);
                glUniform1f(opacity, layer ? 0.5f : 1.0f);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        }
        glFlush();
        bigtime_t t2 = system_time();
        glFinish();
        bigtime_t t3 = system_time();
        if (round >= 5) { upload += t1 - t0; draw += t2 - t1; finish += t3 - t2; }
        bigtime_t spent = t3 - t0;
        if (spent < 16667) snooze(16667 - spent);
    }
    printf("tile %4d: %4d draws (%d layers of %d), upload of %d tiles %.2f ms, draw calls %.2f ms (%.1f us each), finish %.2f ms; error 0x%x\n", tile, tiles * layers, layers, tiles, uploads,
        upload / 1000.0 / rounds, draw / 1000.0 / rounds, double(draw) / rounds / (tiles * layers), finish / 1000.0 / rounds, glGetError());
    return 0;
}
