/* GL 3.2 core regression: check actual draw/readback results for the buffer,
 * texture, instancing and FBO paths used by games. Clear-only tests miss these
 * transport failures. Run independently; does not access game data. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFN(type, name) static type p_##name
GLFN(PFNGLCREATESHADERPROC, glCreateShader);
GLFN(PFNGLSHADERSOURCEPROC, glShaderSource);
GLFN(PFNGLCOMPILESHADERPROC, glCompileShader);
GLFN(PFNGLGETSHADERIVPROC, glGetShaderiv);
GLFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
GLFN(PFNGLCREATEPROGRAMPROC, glCreateProgram);
GLFN(PFNGLATTACHSHADERPROC, glAttachShader);
GLFN(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation);
GLFN(PFNGLLINKPROGRAMPROC, glLinkProgram);
GLFN(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
GLFN(PFNGLUSEPROGRAMPROC, glUseProgram);
GLFN(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
GLFN(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
GLFN(PFNGLGENBUFFERSPROC, glGenBuffers);
GLFN(PFNGLBINDBUFFERPROC, glBindBuffer);
GLFN(PFNGLBUFFERDATAPROC, glBufferData);
GLFN(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
GLFN(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData);
GLFN(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange);
GLFN(PFNGLUNMAPBUFFERPROC, glUnmapBuffer);
GLFN(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
GLFN(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
GLFN(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor);
GLFN(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced);
GLFN(PFNGLMULTIDRAWELEMENTSINDIRECTPROC, glMultiDrawElementsIndirect);
GLFN(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
GLFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
GLFN(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
GLFN(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
GLFN(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer);
GLFN(PFNGLTEXBUFFERPROC, glTexBuffer);
GLFN(PFNGLGETSTRINGIPROC, glGetStringi);

static FILE *report;
static int failures;
static void check_pixel_at(const char *label, int x, int y, unsigned char r, unsigned char g, unsigned char b)
{
    unsigned char pixel[4] = {0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = glGetError();
    int ok = !error && abs(pixel[0] - r) < 5 && abs(pixel[1] - g) < 5 && abs(pixel[2] - b) < 5;
    fprintf(report, "%s %s rgba=%u,%u,%u,%u expected=%u,%u,%u error=%x\n",
            ok ? "PASS" : "FAIL", label, pixel[0], pixel[1], pixel[2], pixel[3], r,g,b,error);
    fflush(report);
    if (!ok) failures++;
}

static void check_pixel(const char *label, unsigned char r, unsigned char g, unsigned char b)
{
    check_pixel_at(label, 32, 32, r, g, b);
}

static GLuint shader(GLenum type, const char *source)
{
    GLuint s = p_glCreateShader(type);
    GLint ok = 0;
    p_glShaderSource(s, 1, &source, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(report, "FAIL shader: %s\n", log);
        failures++;
    }
    return s;
}

int main(void)
{
    report = fopen("C:\\windows\\temp\\winehua-wgl-core-transfer.txt", "w");
    if (!report) return 2;
    setvbuf(report, NULL, _IONBF, 0);
    fprintf(report, "BEGIN pid=%lu\n", GetCurrentProcessId());
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaCoreTransfer";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "OpenGL buffer transfer", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    fprintf(report, "pixel format=%d\n", pf);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return 3;
    HGLRC initial = wglCreateContext(dc);
    fprintf(report, "initial context=%p\n", (void *)initial);
    if (!initial || !wglMakeCurrent(dc, initial)) return 3;
    typedef HGLRC (WINAPI *CreateContext)(HDC, HGLRC, const int *);
    CreateContext create = (CreateContext)wglGetProcAddress("wglCreateContextAttribsARB");
    const int attrs[] = {0x2091,3,0x2092,2,0x9126,1,0};
    HGLRC core = create ? create(dc, NULL, attrs) : NULL;
    if (!core || !wglMakeCurrent(dc, core)) { fprintf(report, "FAIL core context %lu\n", GetLastError()); return 3; }
    wglDeleteContext(initial);
    fprintf(report, "GL=%s renderer=%s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
#define LOAD(name) do { p_##name = (void *)wglGetProcAddress(#name); if (!p_##name) { fprintf(report,"FAIL missing %s\n",#name); return 3; } } while (0)
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv); LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glBindAttribLocation); LOAD(glLinkProgram); LOAD(glGetProgramiv); LOAD(glUseProgram);
    LOAD(glGenVertexArrays); LOAD(glBindVertexArray); LOAD(glGenBuffers); LOAD(glBindBuffer); LOAD(glBufferData); LOAD(glBufferSubData);
    LOAD(glMapBufferRange); LOAD(glUnmapBuffer); LOAD(glEnableVertexAttribArray); LOAD(glVertexAttribPointer); LOAD(glVertexAttribDivisor);
    LOAD(glGetBufferSubData);
    LOAD(glGetStringi);
    LOAD(glDrawElementsInstanced); LOAD(glGenFramebuffers); LOAD(glBindFramebuffer); LOAD(glFramebufferTexture2D);
    LOAD(glMultiDrawElementsIndirect);
    LOAD(glCheckFramebufferStatus); LOAD(glBlitFramebuffer);
    LOAD(glTexBuffer);
    const char *vs = "#version 150\nin vec2 pos; in vec4 col; out vec4 color; void main(){gl_Position=vec4(pos,0,1);color=col;}";
    const char *fs = "#version 150\nin vec4 color; out vec4 output_color; void main(){output_color=color;}";
    GLuint program = p_glCreateProgram();
    p_glAttachShader(program, shader(GL_VERTEX_SHADER, vs)); p_glAttachShader(program, shader(GL_FRAGMENT_SHADER, fs));
    p_glBindAttribLocation(program, 0, "pos"); p_glBindAttribLocation(program, 1, "col");
    p_glLinkProgram(program);
    GLint linked = 0; p_glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { fprintf(report,"FAIL program link\n"); return 3; }
    p_glUseProgram(program);
    GLuint vao, buffers[3]; p_glGenVertexArrays(1,&vao); p_glBindVertexArray(vao); p_glGenBuffers(3,buffers);
    const GLfloat positions[] = {-1,-1, 3,-1, -1,3};
    const GLuint indices[] = {0,1,2};
    const GLfloat colors[][4] = {{1,0,0,1}, {0,1,0,1}, {0,0,1,1}};
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]); p_glBufferData(GL_ARRAY_BUFFER,sizeof(positions),positions,GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]); p_glBufferData(GL_ARRAY_BUFFER,sizeof(colors[0]),colors[0],GL_STREAM_DRAW);
    p_glEnableVertexAttribArray(1); p_glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,0,0); p_glVertexAttribDivisor(1,1);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]); p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);
    GLfloat got_positions[6] = {0}, got_color[4] = {0};
    GLuint got_indices[3] = {0};
    p_glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER,0,sizeof(got_indices),got_indices);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);p_glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof(got_positions),got_positions);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);p_glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof(got_color),got_color);
    int buffer_ok = !memcmp(got_indices,indices,sizeof(indices)) && !memcmp(got_positions,positions,sizeof(positions)) && !memcmp(got_color,colors[0],sizeof(got_color));
    fprintf(report,"%s buffer readback positions=%g,%g,%g,%g,%g,%g color=%g,%g,%g,%g indices=%u,%u,%u\n",buffer_ok?"PASS":"FAIL",
            got_positions[0],got_positions[1],got_positions[2],got_positions[3],got_positions[4],got_positions[5],got_color[0],got_color[1],got_color[2],got_color[3],got_indices[0],got_indices[1],got_indices[2]);
    if(!buffer_ok) failures++;
    glViewport(0,0,64,64); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
    glClearColor(0.25,0.5,0.75,1); glClear(GL_COLOR_BUFFER_BIT); check_pixel("clear",64,128,191);
    p_glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,0,1); check_pixel("BufferData+instancing",255,0,0);
    p_glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(colors[1]),colors[1]);
    p_glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,0,1); check_pixel("BufferSubData",0,255,0);
    void *mapped=p_glMapBufferRange(GL_ARRAY_BUFFER,0,sizeof(colors[2]),GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT);
    if (mapped) {memcpy(mapped,colors[2],sizeof(colors[2])); p_glUnmapBuffer(GL_ARRAY_BUFFER);}
    else {fprintf(report,"FAIL map\n");failures++;}
    p_glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,0,1); check_pixel("MapBufferRange",0,0,255);
    GLuint texture, fbo; glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    unsigned char texels[64*64*4];
    for(unsigned i=0;i<sizeof(texels);i+=4) {texels[i]=255;texels[i+1]=128;texels[i+2]=0;texels[i+3]=255;}
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,texels);
    p_glGenFramebuffers(1,&fbo); p_glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    if(p_glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {fprintf(report,"FAIL FBO\n");return 3;}
    check_pixel("texture upload",255,128,0);
    p_glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,0,1);check_pixel("FBO draw",0,0,255);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    p_glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER,0);check_pixel("FBO blit to default",0,0,255);
    /* Sampling and sub-image updates exercise descriptor setup and atlas
     * uploads separately from using a texture as a render attachment. */
    GLuint sampled_program = p_glCreateProgram();
    const char *sample_fs = "#version 150\nuniform sampler2D tex; out vec4 output_color; void main(){output_color=texture(tex,gl_FragCoord.xy/64.0);}";
    p_glAttachShader(sampled_program,shader(GL_VERTEX_SHADER,vs));
    p_glAttachShader(sampled_program,shader(GL_FRAGMENT_SHADER,sample_fs));
    p_glBindAttribLocation(sampled_program,0,"pos");p_glLinkProgram(sampled_program);
    p_glGetProgramiv(sampled_program,GL_LINK_STATUS,&linked);
    if(!linked) {fprintf(report,"FAIL sampled program link\n");return 3;}
    p_glUseProgram(sampled_program);
    p_glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,0,1);
    check_pixel("texture sampling",0,0,255);
    for(unsigned i=0;i<sizeof(texels);i+=4) {texels[i]=0;texels[i+1]=255;texels[i+2]=0;texels[i+3]=255;}
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,texels);
    p_glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,0,1);
    check_pixel("texture atlas update",0,255,0);
    unsigned char padded[24*13*4];
    for(unsigned i=0;i<sizeof(padded);i+=4){padded[i]=255;padded[i+1]=0;padded[i+2]=255;padded[i+3]=255;}
    glPixelStorei(GL_UNPACK_ROW_LENGTH,24);
    glTexSubImage2D(GL_TEXTURE_2D,0,10,12,19,13,GL_RGBA,GL_UNSIGNED_BYTE,padded);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    p_glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    check_pixel_at("texture padded subregion",18,18,255,0,255);
    check_pixel_at("texture surrounding pixels preserved",5,5,0,255,0);
    p_glBindFramebuffer(GL_FRAMEBUFFER,0);
    /* A quad requires both indexed triangles. Non-zero buffer offsets catch
     * transfers that accidentally copy only the start of a shared slab. */
    const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    const GLushort quad_indices[] = {0,1,2, 2,1,3};
    p_glUseProgram(program);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
    p_glBufferData(GL_ARRAY_BUFFER,8192,NULL,GL_STREAM_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER,1024,sizeof(quad),quad);
    p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,(void *)1024);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);
    p_glBufferData(GL_ARRAY_BUFFER,4096,NULL,GL_STREAM_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER,256,sizeof(colors[0]),colors[0]);
    p_glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,0,(void *)256);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,4096,NULL,GL_STREAM_DRAW);
    p_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,128,sizeof(quad_indices),quad_indices);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    p_glDrawElementsInstanced(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,(void *)128,1);
    check_pixel_at("offset quad first triangle",16,16,255,0,0);
    check_pixel_at("offset quad second triangle",48,48,255,0,0);
    /* Match a game's interleaved position/color vertices and 84-byte
     * per-instance transform/color/depth records, rather than one constant
     * attribute. The two instances must cover opposite halves of the quad. */
    const char *matrix_vs = "#version 150\nin vec2 pos;in vec4 col;in vec4 m0;in vec4 m1;in vec4 m2;in vec4 mul;in vec4 add;in float bias;out vec4 color;out float depth_bias;void main(){gl_Position=m0*pos.x+m1*pos.y+m2;color=col*mul+add;depth_bias=bias;}";
    const char *matrix_fs = "#version 150\nin vec4 color;in float depth_bias;out vec4 output_color;void main(){output_color=color;gl_FragDepth=gl_FragCoord.z-depth_bias*0.001;}";
    GLuint matrix_program=p_glCreateProgram();
    p_glAttachShader(matrix_program,shader(GL_VERTEX_SHADER,matrix_vs));
    p_glAttachShader(matrix_program,shader(GL_FRAGMENT_SHADER,matrix_fs));
    const char *names[]={"pos","col","m0","m1","m2","mul","add","bias"};
    for(unsigned i=0;i<8;i++)p_glBindAttribLocation(matrix_program,i,names[i]);
    p_glLinkProgram(matrix_program);p_glGetProgramiv(matrix_program,GL_LINK_STATUS,&linked);
    if(!linked){fprintf(report,"FAIL matrix program link\n");return 3;}
    p_glUseProgram(matrix_program);
    struct packed_vertex {GLfloat x,y;unsigned char color[4];};
    const struct packed_vertex packed_vertices[]={ {-1,-1,{255,255,255,255}}, {1,-1,{255,255,255,255}}, {-1,1,{255,255,255,255}}, {1,1,{255,255,255,255}} };
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
    p_glBufferData(GL_ARRAY_BUFFER,8192,NULL,GL_STREAM_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER,1024,sizeof(packed_vertices),packed_vertices);
    p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(struct packed_vertex),(void*)1024);
    p_glVertexAttribPointer(1,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(struct packed_vertex),(void*)1032);
    p_glVertexAttribDivisor(1,0);
    const GLfloat instances[2][21]={
      {0.5,0,0,0, 0,1,0,0, -0.5,0,0,1, 1,0,0,1, 0,0,0,0, 0},
      {0.5,0,0,0, 0,1,0,0, 0.5,0,0,1, 0,1,0,1, 0,0,0,0, 0}
    };
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);
    p_glBufferData(GL_ARRAY_BUFFER,8192,NULL,GL_STREAM_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER,512,sizeof(instances),instances);
    for(unsigned i=2;i<8;i++) {
      p_glEnableVertexAttribArray(i);
      p_glVertexAttribPointer(i,i==7?1:4,GL_FLOAT,GL_FALSE,sizeof(instances[0]),(void*)(uintptr_t)(512+(i-2)*16));
      p_glVertexAttribDivisor(i,1);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    p_glDrawElementsInstanced(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,(void*)128,2);
    check_pixel_at("matrix instance left lower",8,16,255,0,0);
    check_pixel_at("matrix instance left upper",24,48,255,0,0);
    check_pixel_at("matrix instance right lower",40,16,0,255,0);
    check_pixel_at("matrix instance right upper",56,48,0,255,0);
    struct indirect_draw {GLuint count,instances,first_index;GLint base_vertex;GLuint base_instance;};
    const struct indirect_draw commands[]={{6,1,64,0,0},{6,1,64,0,1}};
    GLuint indirect_buffer;p_glGenBuffers(1,&indirect_buffer);p_glBindBuffer(GL_DRAW_INDIRECT_BUFFER,indirect_buffer);
    p_glBufferData(GL_DRAW_INDIRECT_BUFFER,sizeof(commands),commands,GL_STREAM_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_SHORT,0,2,0);
    check_pixel_at("indirect first instance lower",8,16,255,0,0);
    check_pixel_at("indirect first instance upper",24,48,255,0,0);
    check_pixel_at("indirect base instance lower",40,16,0,255,0);
    check_pixel_at("indirect base instance upper",56,48,0,255,0);
    const struct indirect_draw command_update={3,1,67,0,1};
    p_glBufferSubData(GL_DRAW_INDIRECT_BUFFER,20,sizeof(command_update),&command_update);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_SHORT,(void*)20,1,0);
    check_pixel_at("indirect nonzero offset",56,48,0,255,0);
    GLfloat instances100[2][25]={{0}};
    for(unsigned i=0;i<2;i++){memcpy(instances100[i],instances[i],20*sizeof(GLfloat));instances100[i][24]=instances[i][20];}
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);p_glBufferData(GL_ARRAY_BUFFER,8192,NULL,GL_STREAM_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER,512,sizeof(instances100),instances100);
    for(unsigned i=2;i<8;i++)p_glVertexAttribPointer(i,i==7?1:4,GL_FLOAT,GL_FALSE,100,(void*)(uintptr_t)(512+(i==7?96:(i-2)*16)));
    const struct indirect_draw offset_commands[]={{6,1,64,4,0},{6,1,64,4,1}};
    p_glBufferData(GL_DRAW_INDIRECT_BUFFER,sizeof(offset_commands),offset_commands,GL_STREAM_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
    p_glBufferSubData(GL_ARRAY_BUFFER,1024+4*sizeof(struct packed_vertex),sizeof(packed_vertices),packed_vertices);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_SHORT,0,2,0);
    check_pixel_at("100 byte stride base vertex left",8,16,255,0,0);
    check_pixel_at("100 byte stride base vertex right",56,48,0,255,0);
    const struct indirect_draw one_draw={6,1,64,4,0};
    p_glBufferData(GL_DRAW_INDIRECT_BUFFER,sizeof(one_draw),&one_draw,GL_STREAM_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);
    p_glBufferSubData(GL_ARRAY_BUFFER,512,sizeof(instances100[0]),instances100[0]);
    p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_SHORT,0,1,0);
    p_glBufferSubData(GL_ARRAY_BUFFER,512,sizeof(instances100[1]),instances100[1]);
    p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_SHORT,0,1,0);
    check_pixel_at("overwrite after queued draw preserves old instance",8,16,255,0,0);
    check_pixel_at("overwrite after queued draw uses new instance",56,48,0,255,0);
    p_glBufferSubData(GL_ARRAY_BUFFER,512,sizeof(instances100),instances100);
    const GLuint indices32[]={0,1,2,2,1,3};
    const struct indirect_draw commands32[]={{6,1,32,4,0},{6,1,32,4,1}};
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,4096,NULL,GL_DYNAMIC_DRAW);
    p_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,128,sizeof(indices32),indices32);
    p_glBufferData(GL_DRAW_INDIRECT_BUFFER,sizeof(commands32),commands32,GL_DYNAMIC_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_INT,0,2,0);
    check_pixel_at("32 bit indirect base vertex left",8,16,255,0,0);
    check_pixel_at("32 bit indirect base vertex right",56,48,0,255,0);
    for(unsigned iteration=0;iteration<160;iteration++) {
      p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);
      p_glBufferSubData(GL_ARRAY_BUFFER,512,sizeof(instances100),instances100);
      p_glBufferSubData(GL_DRAW_INDIRECT_BUFFER,0,sizeof(commands32),commands32);
      p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_INT,0,2,0);
    }
    check_pixel_at("reused indirect and instance buffer left",8,16,255,0,0);
    check_pixel_at("reused indirect and instance buffer right",56,48,0,255,0);
    /* Match the exact 40-byte float vertex layout seen in Mewgenics:
     * vec2 position, vec4 color at byte 8, vec4 texcoord at byte 24. */
    const char *exact_vs="#version 150\nin vec2 pos;in vec4 col;in vec4 tex;in vec4 m0;in vec4 m1;in vec4 m2;in vec4 mul;in vec4 add;in vec4 extra;in float bias;out vec4 color;out float depth_bias;void main(){gl_Position=m0*pos.x+m1*pos.y+m2;color=col*mul+add+tex+extra;depth_bias=bias;}";
    GLuint exact_program=p_glCreateProgram();p_glAttachShader(exact_program,shader(GL_VERTEX_SHADER,exact_vs));
    p_glAttachShader(exact_program,shader(GL_FRAGMENT_SHADER,matrix_fs));
    const char *exact_names[]={"pos","col","tex","m0","m1","m2","mul","add","extra","bias"};
    for(unsigned i=0;i<10;i++)p_glBindAttribLocation(exact_program,i,exact_names[i]);
    p_glLinkProgram(exact_program);p_glGetProgramiv(exact_program,GL_LINK_STATUS,&linked);if(!linked)return 3;
    p_glUseProgram(exact_program);
    GLfloat exact_vertices[4][10]={{0}};
    for(unsigned i=0;i<4;i++){exact_vertices[i][0]=quad[2*i];exact_vertices[i][1]=quad[2*i+1];for(unsigned j=2;j<6;j++)exact_vertices[i][j]=1;}
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);p_glBufferData(GL_ARRAY_BUFFER,8192,NULL,GL_STATIC_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER,160,sizeof(exact_vertices),exact_vertices);
    for(unsigned i=0;i<3;i++){p_glEnableVertexAttribArray(i);p_glVertexAttribPointer(i,i==0?2:4,GL_FLOAT,GL_FALSE,40,(void*)(uintptr_t)(i==0?0:8+(i-1)*16));p_glVertexAttribDivisor(i,0);}
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);p_glBufferData(GL_ARRAY_BUFFER,sizeof(instances100),instances100,GL_DYNAMIC_DRAW);
    for(unsigned i=3;i<10;i++){p_glEnableVertexAttribArray(i);p_glVertexAttribPointer(i,i==9?1:4,GL_FLOAT,GL_FALSE,100,(void*)(uintptr_t)((i-3)*16));p_glVertexAttribDivisor(i,1);}
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]);p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices32),indices32,GL_STATIC_DRAW);
    const struct indirect_draw exact_commands[]={{6,1,0,4,0},{6,1,0,4,1}};
    p_glBufferData(GL_DRAW_INDIRECT_BUFFER,sizeof(exact_commands),exact_commands,GL_DYNAMIC_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_INT,0,2,0);
    check_pixel_at("40 byte vertex 100 byte instance left lower",8,16,255,0,0);
    check_pixel_at("40 byte vertex 100 byte instance left upper",24,48,255,0,0);
    check_pixel_at("40 byte vertex 100 byte instance right lower",40,16,0,255,0);
    check_pixel_at("40 byte vertex 100 byte instance right upper",56,48,0,255,0);
    /* Matrices reserve consecutive attribute locations even when the shader
     * does not use one column. Reproduce the unused MVP column in a 2D game. */
    const char *matrix_gap_vs="#version 150\nin vec2 pos;in vec4 col;in vec4 tex;in mat4 mvp;in mat2x4 color_xform;in float bias;out vec4 color;out float depth_bias;void main(){gl_Position=mvp*vec4(pos,0,1);color=col*color_xform[0]+color_xform[1]+tex;depth_bias=bias;}";
    GLuint gap_program=p_glCreateProgram();p_glAttachShader(gap_program,shader(GL_VERTEX_SHADER,matrix_gap_vs));
    p_glAttachShader(gap_program,shader(GL_FRAGMENT_SHADER,matrix_fs));
    p_glBindAttribLocation(gap_program,0,"pos");p_glBindAttribLocation(gap_program,1,"col");p_glBindAttribLocation(gap_program,2,"tex");
    p_glBindAttribLocation(gap_program,3,"mvp");p_glBindAttribLocation(gap_program,7,"color_xform");p_glBindAttribLocation(gap_program,9,"bias");
    p_glLinkProgram(gap_program);p_glGetProgramiv(gap_program,GL_LINK_STATUS,&linked);if(!linked)return 3;
    GLfloat matrix_gap_data[2][25]={{0}};
    for(unsigned i=0;i<2;i++) {
      memcpy(matrix_gap_data[i],instances[i],8*sizeof(GLfloat));
      matrix_gap_data[i][10]=1;
      memcpy(matrix_gap_data[i]+12,instances[i]+8,4*sizeof(GLfloat));
      memcpy(matrix_gap_data[i]+16,instances[i]+12,8*sizeof(GLfloat));
    }
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);p_glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(matrix_gap_data),matrix_gap_data);
    p_glUseProgram(gap_program);glClear(GL_COLOR_BUFFER_BIT);
    p_glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_INT,0,2,0);
    check_pixel_at("matrix unused column left lower",8,16,255,0,0);
    check_pixel_at("matrix unused column left upper",24,48,255,0,0);
    check_pixel_at("matrix unused column right lower",40,16,0,255,0);
    check_pixel_at("matrix unused column right upper",56,48,0,255,0);
    /* Exceed small-upload thresholds and 16K vertices with 32-bit indices. */
    const unsigned columns=80, rows=75, quads=columns*rows;
    GLfloat *grid=malloc(quads*8*sizeof(GLfloat));
    GLuint *grid_indices=malloc(quads*6*sizeof(GLuint));
    if(!grid||!grid_indices)return 3;
    for(unsigned q=0;q<quads;q++) {
      float x=-1+2.0f*(q%columns)/columns,y=-1+2.0f*(q/columns)/rows;
      const GLfloat points[]={x,y,x+2.0f/columns,y,x,y+2.0f/rows,x+2.0f/columns,y+2.0f/rows};
      memcpy(grid+q*8,points,sizeof(points));
      for(unsigned i=0;i<6;i++)grid_indices[q*6+i]=q*4+quad_indices[i];
    }
    p_glUseProgram(program);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);p_glBufferData(GL_ARRAY_BUFFER,quads*8*sizeof(GLfloat),grid,GL_STREAM_DRAW);
    p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[1]);p_glBufferData(GL_ARRAY_BUFFER,sizeof(colors[0]),colors[0],GL_STREAM_DRAW);
    p_glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,0,0);p_glVertexAttribDivisor(1,1);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]);p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,quads*6*sizeof(GLuint),grid_indices,GL_STREAM_DRAW);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glDrawElementsInstanced(GL_TRIANGLES,quads*6,GL_UNSIGNED_INT,0,1);
    check_pixel_at("large indexed mesh lower",8,16,255,0,0);
    check_pixel_at("large indexed mesh upper",56,48,255,0,0);
    /* Publish many disjoint writes before any draw or readback. A shadow
     * invalidate for a later write must not discard earlier queued writes. */
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
    p_glBufferData(GL_ARRAY_BUFFER,quads*8*sizeof(GLfloat),NULL,GL_STREAM_DRAW);
    for(unsigned q=0;q<quads;q+=40)
      p_glBufferSubData(GL_ARRAY_BUFFER,q*8*sizeof(GLfloat),40*8*sizeof(GLfloat),grid+q*8);
    p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,quads*6*sizeof(GLuint),NULL,GL_STREAM_DRAW);
    for(unsigned q=0;q<quads;q+=40)
      p_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,q*6*sizeof(GLuint),40*6*sizeof(GLuint),grid_indices+q*6);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glDrawElementsInstanced(GL_TRIANGLES,quads*6,GL_UNSIGNED_INT,0,1);
    check_pixel_at("segmented mesh lower",8,16,255,0,0);
    check_pixel_at("segmented mesh upper",56,48,255,0,0);
    /* Game-sized static buffers use a different device-local upload path
     * from small streaming buffers. Keep updates far from allocation start. */
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
    p_glBufferData(GL_ARRAY_BUFFER,83886080,NULL,GL_STATIC_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER,40*1024*1024,quads*8*sizeof(GLfloat),grid);
    p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,(void*)(40*1024*1024));
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,33554432,NULL,GL_STATIC_DRAW);
    p_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,20*1024*1024,quads*6*sizeof(GLuint),grid_indices);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glDrawElementsInstanced(GL_TRIANGLES,quads*6,GL_UNSIGNED_INT,(void*)(20*1024*1024),1);
    check_pixel_at("large static buffer lower",8,16,255,0,0);
    check_pixel_at("large static buffer upper",56,48,255,0,0);
    /* Restore the small buffers for the remaining clipping checks. */
    p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);p_glBufferData(GL_ARRAY_BUFFER,quads*8*sizeof(GLfloat),grid,GL_STREAM_DRAW);
    p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[2]);p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,quads*6*sizeof(GLuint),grid_indices,GL_STREAM_DRAW);
    glClearColor(0,0,0,1);glClearStencil(0);glStencilMask(255);
    glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);glStencilFunc(GL_ALWAYS,12,255);glStencilOp(GL_KEEP,GL_REPLACE,GL_REPLACE);
    glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glEnable(GL_SCISSOR_TEST);glScissor(0,0,32,64);
    p_glDrawElementsInstanced(GL_TRIANGLES,quads*6,GL_UNSIGNED_INT,0,1);
    glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glStencilMask(0);glStencilFunc(GL_EQUAL,12,255);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
    p_glDrawElementsInstanced(GL_TRIANGLES,quads*6,GL_UNSIGNED_INT,0,1);
    check_pixel_at("stencil kept lower",8,16,255,0,0);check_pixel_at("stencil kept upper",24,48,255,0,0);
    check_pixel_at("stencil clipped lower",40,16,0,0,0);check_pixel_at("stencil clipped upper",56,48,0,0,0);
    glDisable(GL_STENCIL_TEST);glStencilMask(255);
    const char *table_vs="#version 150\nin vec2 pos;uniform samplerBuffer table;out vec4 color;void main(){int k=1800;gl_Position=vec4(pos,0,1);color=vec4(texelFetch(table,k).r,texelFetch(table,k+1).r,texelFetch(table,k+2).r,texelFetch(table,k+3).r);}";
    GLuint table_program=p_glCreateProgram();p_glAttachShader(table_program,shader(GL_VERTEX_SHADER,table_vs));
    p_glAttachShader(table_program,shader(GL_FRAGMENT_SHADER,fs));p_glBindAttribLocation(table_program,0,"pos");
    p_glLinkProgram(table_program);p_glGetProgramiv(table_program,GL_LINK_STATUS,&linked);if(!linked)return 3;
    p_glUseProgram(table_program);
    GLuint table_buffer,table_texture;p_glGenBuffers(1,&table_buffer);glGenTextures(1,&table_texture);
    p_glBindBuffer(GL_TEXTURE_BUFFER,table_buffer);p_glBufferData(GL_TEXTURE_BUFFER,36300,NULL,GL_STATIC_DRAW);
    const GLfloat table_color[]={0,1,0,1};p_glBufferSubData(GL_TEXTURE_BUFFER,7200,sizeof(table_color),table_color);
    glBindTexture(GL_TEXTURE_BUFFER,table_texture);p_glTexBuffer(GL_TEXTURE_BUFFER,GL_R32F,table_buffer);
    glClear(GL_COLOR_BUFFER_BIT);p_glDrawElementsInstanced(GL_TRIANGLES,quads*6,GL_UNSIGNED_INT,0,1);
    check_pixel_at("R32F atlas table left",8,16,0,255,0);check_pixel_at("R32F atlas table right",56,48,0,255,0);
    /* Optional soak of the real Wine SwapBuffers readback bridge. The small
     * pixel checks above alone cannot expose memory retained per presentation.
     * Sample process RSS/Swap externally; the frame counter identifies progress
     * independently of any FPS file left by a previous process. */
    const char *soak_option = getenv("WINEHUA_GL_SOAK_FRAMES");
    int soak_frames = soak_option ? atoi(soak_option) : 0;
    if (soak_frames < 0 || soak_frames > 20000) return 3;
    if (soak_frames) {
        DWORD started = GetTickCount();
        int pack_state_test = getenv("WINEHUA_GL_SOAK_PACK_STATE") &&
                              !strcmp(getenv("WINEHUA_GL_SOAK_PACK_STATE"), "1");
        if (pack_state_test) {
            GLint extensions = 0;
            int supported = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
            for (GLint i = 0; i < extensions; ++i) {
                const char *name = (const char *)p_glGetStringi(GL_EXTENSIONS, i);
                if (name && !strcmp(name, "GL_MESA_pack_invert")) supported = 1;
            }
            fprintf(report, "%s GL_MESA_pack_invert=%d\n", supported ? "PASS" : "FAIL", supported);
            if (!supported) { failures++; goto finish; }
        }
        RECT client;
        const char *size_option = getenv("WINEHUA_GL_SOAK_SIZE");
        int width, height;
        if (size_option) {
            if (sscanf(size_option, "%dx%d", &width, &height) != 2 ||
                width < 64 || height < 64 || width > 4096 || height > 4096) return 3;
            RECT outer = {0, 0, width, height};
            AdjustWindowRectEx(&outer, GetWindowLongA(window, GWL_STYLE), FALSE,
                               GetWindowLongA(window, GWL_EXSTYLE));
            SetWindowPos(window, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                         SWP_NOMOVE | SWP_NOZORDER);
            /* Wine recreates the drawable at the first swap after resize.
             * Complete that transition before checking the larger viewport. */
            if (!SwapBuffers(dc)) return 3;
        }
        GetClientRect(window, &client);
        glViewport(0, 0, client.right, client.bottom);
        fprintf(report, "SOAK BEGIN frames=%d size=%ldx%ld\n",
                soak_frames, client.right, client.bottom);
        for (int frame = 1; frame <= soak_frames; frame++) {
            MSG message;
            while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            if (!IsWindow(window)) {
                fprintf(report, "SOAK INTERRUPTED frame=%d\n", frame);
                fclose(report);
                return 4;
            }
            p_glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);
            p_glBufferData(GL_ARRAY_BUFFER,quads*8*sizeof(GLfloat),grid,GL_STREAM_DRAW);
            const GLfloat soak_color[] = {(frame & 1) ? 1 : 0, (frame & 1) ? 0 : 1, 0, 1};
            p_glBindBuffer(GL_TEXTURE_BUFFER,table_buffer);
            p_glBufferSubData(GL_TEXTURE_BUFFER,7200,sizeof(soak_color),soak_color);
            glClear(GL_COLOR_BUFFER_BIT);
            p_glDrawElementsInstanced(GL_TRIANGLES,quads*6,GL_UNSIGNED_INT,0,1);
            if (!(frame % 120) || frame % 120 == 1) {
                check_pixel_at("soak draw", client.right / 2, client.bottom / 2,
                               (frame & 1) ? 255 : 0, (frame & 1) ? 0 : 255, 0);
                fprintf(report, "SOAK frame=%d elapsed_ms=%lu\n",
                        frame, GetTickCount() - started);
            }
            if (pack_state_test) glPixelStorei(GL_PACK_INVERT_MESA, frame & 1);
            if (!SwapBuffers(dc)) {
                fprintf(report, "FAIL soak SwapBuffers frame=%d error=%lu\n", frame, GetLastError());
                failures++;
                break;
            }
            if (pack_state_test) {
                GLint restored = -1;
                glGetIntegerv(GL_PACK_INVERT_MESA, &restored);
                GLenum error = glGetError();
                glPixelStorei(GL_PACK_INVERT_MESA, GL_FALSE);
                if (restored != (frame & 1) || error) {
                    fprintf(report, "FAIL pack invert restore frame=%d value=%d error=%x\n", frame, restored, error);
                    failures++;
                    break;
                }
            }
        }
        if (getenv("WINEHUA_GL_SOAK_QUADRANTS") &&
            !strcmp(getenv("WINEHUA_GL_SOAK_QUADRANTS"), "1")) {
            /* Upper-left red, upper-right green, lower-left blue, lower-right
             * yellow. A screenshot checks the real display and Y orientation. */
            p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_DEPTH_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable(GL_SCISSOR_TEST);
            const GLfloat corner_colors[4][4] = {{0,0,1,1}, {1,1,0,1}, {1,0,0,1}, {0,1,0,1}};
            for (int q = 0; q != 4; ++q) {
                int x0 = (q & 1) ? client.right / 2 : 0;
                int y0 = (q & 2) ? client.bottom / 2 : 0;
                glScissor(x0, y0, (q & 1) ? client.right - x0 : client.right / 2,
                          (q & 2) ? client.bottom - y0 : client.bottom / 2);
                glClearColor(corner_colors[q][0], corner_colors[q][1], corner_colors[q][2], 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            fprintf(report, "QUADRANTS top=red,green bottom=blue,yellow\n");
        }
    }
finish:
    free(grid);free(grid_indices);
    fprintf(report,"RESULT failures=%d\n",failures);
    SwapBuffers(dc);
    fclose(report);
    Sleep(10000);
    wglMakeCurrent(NULL,NULL);wglDeleteContext(core);ReleaseDC(window,dc);DestroyWindow(window);
    return failures != 0;
}
