// egltime: cost of EGL start-up as a web process does it: display, init,
// config, pbuffer, GLES context, make current, compile + link a shader pair.
#include <OS.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <vector>
static bigtime_t last;
static void mark(const char* what) { bigtime_t now = system_time(); std::printf("%-22s %8.1f ms\n", what, (now - last) / 1e3); last = now; }
int main()
{
    bigtime_t start = last = system_time();
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY); mark("eglGetDisplay");
    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) { std::printf("eglInitialize failed %x\n", eglGetError()); return 1; }
    mark("eglInitialize");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config; EGLint count = 0;
    eglChooseConfig(display, attributes, &config, 1, &count); mark("eglChooseConfig");
    const EGLint pbufferAttributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttributes); mark("pbuffer");
    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes); mark("eglCreateContext");
    eglMakeCurrent(display, surface, surface, context); mark("eglMakeCurrent");
    const char* vs = "attribute vec4 p; attribute vec2 t; varying vec2 v; uniform mat4 m; void main(){ v = t; gl_Position = m * p; }";
    const char* fs = "precision mediump float; varying vec2 v; uniform sampler2D s; uniform float o; void main(){ gl_FragColor = texture2D(s, v) * o; }";
    GLuint program = glCreateProgram();
    GLuint a = glCreateShader(GL_VERTEX_SHADER); glShaderSource(a, 1, &vs, nullptr); glCompileShader(a);
    GLuint b = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(b, 1, &fs, nullptr); glCompileShader(b);
    glAttachShader(program, a); glAttachShader(program, b); glLinkProgram(program);
    GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok); mark(ok ? "compile+link" : "compile+link FAILED");
    glUseProgram(program); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, 3); glFinish(); mark("first draw+finish");
    GLint formats = 0; glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    std::printf("program binary formats: %d\n", formats);
    if (formats > 0) {
        GLint length = 0; glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        std::vector<char> binary(length); GLenum format = 0; GLsizei written = 0;
        glGetProgramBinary(program, length, &written, &format, binary.data());
        mark("get binary");
        GLuint again = glCreateProgram();
        glProgramBinary(again, format, binary.data(), written);
        GLint ok2 = 0; glGetProgramiv(again, GL_LINK_STATUS, &ok2);
        mark(ok2 ? "program from binary" : "program from binary FAILED");
        glUseProgram(again); glDrawArrays(GL_TRIANGLES, 0, 3); glFinish(); mark("draw with binary program");
        std::printf("binary %d bytes\n", written);
    }
    std::printf("total %.1f ms, EGL %d.%d, %s / %s\n", (system_time() - start) / 1e3, major, minor,
        (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    return 0;
}
