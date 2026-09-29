// What does uploading a batch of painted tiles cost on zink?
// upload-probe [tiles tileSize rounds]
// build: g++ -O2 -o upload-probe upload-probe.cpp -I$PREFIX/include -L$PREFIX/lib -lEGL -lGLESv2 -lbe
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    int tiles = argc > 1 ? atoi(argv[1]) : 384;
    int size = argc > 2 ? atoi(argv[2]) : 256;
    int rounds = argc > 3 ? atoi(argv[3]) : 10;
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
    printf("GL_RENDERER=%s, %d tiles of %dx%d (%.1f Mpx, %.1f MiB)\n", glGetString(GL_RENDERER), tiles, size, size, tiles * double(size) * size / 1e6, tiles * double(size) * size * 4 / 1048576);

    size_t tileBytes = size_t(size) * size * 4;
    std::vector<uint8_t> pixels(tileBytes * tiles);
    for (size_t i = 0; i < pixels.size(); i += 4096) pixels[i] = uint8_t(i);

    bigtime_t t0 = system_time();
    std::vector<uint8_t> copy(pixels.size());
    memcpy(copy.data(), pixels.data(), pixels.size());
    printf("memcpy of all tiles (first touch of the destination): %.1f ms\n", (system_time() - t0) / 1000.0);
    t0 = system_time();
    memcpy(copy.data(), pixels.data(), pixels.size());
    printf("memcpy of all tiles again: %.1f ms\n", (system_time() - t0) / 1000.0);

    std::vector<GLuint> textures(tiles);
    t0 = system_time();
    glGenTextures(tiles, textures.data());
    for (GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    bigtime_t created = system_time();
    glFinish();
    printf("creating the textures: %.1f ms (%.0f us each), finish %.1f ms\n", (created - t0) / 1000.0, double(created - t0) / tiles, (system_time() - created) / 1000.0);

    for (int pass = 0; pass < 4; ++pass) {
        int format = pass & 1;
        GLenum glFormat = format ? GL_BGRA_EXT : GL_RGBA;
        {
            // BGRA data needs BGRA textures in GLES.
            for (GLuint texture : textures) {
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, glFormat, size, size, 0, glFormat, GL_UNSIGNED_BYTE, nullptr);
            }
            glFinish();
        }
        bigtime_t uploadTotal = 0, finishTotal = 0, worst = 0;
        for (int round = 0; round < rounds; ++round) {
            pixels[round] ^= 0xff;
            bigtime_t start = system_time();
            for (int i = 0; i < tiles; ++i) {
                glBindTexture(GL_TEXTURE_2D, textures[i]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, glFormat, GL_UNSIGNED_BYTE, pixels.data() + tileBytes * i);
            }
            bigtime_t uploaded = system_time();
            glFinish();
            bigtime_t finished = system_time();
            uploadTotal += uploaded - start;
            finishTotal += finished - uploaded;
            if (uploaded - start > worst) worst = uploaded - start;
            printf("  round %d: %.1f ms\n", round, (uploaded - start) / 1000.0);
            snooze(argc > 4 ? atoi(argv[4]) * 1000 : 20000);
        }
        printf("%s uploads: %.1f ms a batch (%.0f us a tile, worst batch %.1f ms), then finish %.1f ms; error 0x%x\n", format ? "BGRA" : "RGBA",
            uploadTotal / 1000.0 / rounds, double(uploadTotal) / rounds / tiles, worst / 1000.0, finishTotal / 1000.0 / rounds, glGetError());
    }
    return 0;
}
