// Verify the production readback-band policy against a live EGL context.
// readback-policy.inc is extracted from AcceleratedSurface.cpp by build-probes.sh.
// Usage: readback-policy EXPECTED_ROWS (1661-pixel-wide frame; 384 KiB = 59 rows).
// Run in fresh processes to test the cached policy with different environment settings.
#include <OS.h>
#include <image.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "readback-policy.inc"
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API)) return 3;
    const EGLint ca[] = { EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE };
    EGLConfig c; EGLint n;
    if (!eglChooseConfig(d,ca,&c,1,&n) || !n) return 4;
    const EGLint pa[] = { EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d,c,pa);
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE };
    EGLContext x = eglCreateContext(d,c,EGL_NO_CONTEXT,xa);
    if (!eglMakeCurrent(d,s,s,x)) return 5;
    constexpr int width=1661,height=798;
    int rows=readbackBandRowsHaiku(width), expected=atoi(argv[1]);
    printf("renderer=%s selectedRows=%d expectedRows=%d\n",glGetString(GL_RENDERER),rows,expected);
    if (rows != expected) return 6;
    GLuint tex,fbo;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 7;
    std::vector<unsigned char> out(size_t(width)*height*4,0);
    const unsigned char colors[4][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0}};
    size_t checks=0,errors=0;
    glEnable(GL_SCISSOR_TEST);
    for (int frame=0;frame<16;frame++) {
        for (int xx=0;xx<width;xx+=83) {
            auto& color=colors[(xx/83+frame)%4];
            glScissor(xx,0,std::min(83,width-xx),height);
            glClearColor(color[0]/255.f,color[1]/255.f,color[2]/255.f,1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        const int step=rows?rows:height;
        glPixelStorei(GL_PACK_ALIGNMENT,4);
        glPixelStorei(GL_PACK_ROW_LENGTH,width);
        for (int yy=0;yy<height;yy+=step) {
            glPixelStorei(GL_PACK_SKIP_ROWS,yy);
            glReadPixels(0,yy,width,std::min(step,height-yy),0x80e1,GL_UNSIGNED_BYTE,out.data());
        }
        glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);
        if (glGetError()!=GL_NO_ERROR) return 8;
        for (int yy=0;yy<height;yy++) for (int xx=0;xx<width;xx++) {
            auto& color=colors[(xx/83+frame)%4];auto* p=&out[(size_t(yy)*width+xx)*4];
            errors += p[0]!=color[2] || p[1]!=color[1] || p[2]!=color[0] || p[3]!=255;
            checks++;
        }
    }
    printf("pixelsChecked=%zu errors=%zu\n",checks,errors);
    glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&tex);
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(d,x);eglDestroySurface(d,s);eglTerminate(d);
    return errors?9:0;
}
