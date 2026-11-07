#pragma once

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <VrApi.h>
#include <VrApi_Helpers.h>
#include <cmath>
#include <stdexcept>

namespace govr {

/* A flat rectangle facing +Z (towards the user), in VrApi's tracking space. */
struct Quad {
    float cx = 0, cy = 0, cz = 0, w = 0, h = 0;
    bool visible = false;

    bool Hit(const ovrVector3f &origin, const ovrVector3f &direction, float *distance, float *u, float *v) const {
        if (!visible || direction.z > -1e-4f) return false;
        const float t = (cz - origin.z) / direction.z;
        if (t <= 0) return false;
        const float x = origin.x + direction.x * t, y = origin.y + direction.y * t;
        const float hu = (x - (cx - w * 0.5f)) / w, hv = ((cy + h * 0.5f) - y) / h;
        if (hu < 0 || hu > 1 || hv < 0 || hv > 1) return false;
        *distance = t;
        *u = hu;
        *v = hv;
        return true;
    }
};

class Home {
public:
    void Create(int width, int height) {
        width_ = width;
        height_ = height;
        for (int eye = 0; eye < 2; eye++) {
            Eye &target = eyes_[eye];
            target.chain = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, GL_SRGB8_ALPHA8, width, height, 1, 3);
            if (!target.chain) throw std::runtime_error("home swapchain unavailable");
            target.length = vrapi_GetTextureSwapChainLength(target.chain);
            glGenRenderbuffers(1, &target.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
            glGenFramebuffers(target.length, target.framebuffers);
            for (int index = 0; index < target.length; index++) {
                glBindTexture(GL_TEXTURE_2D, vrapi_GetTextureSwapChainHandle(target.chain, index));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffers[index]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       vrapi_GetTextureSwapChainHandle(target.chain, index), 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    throw std::runtime_error("home framebuffer incomplete");
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        floorProgram_ = Program(kQuadVertex, kFloorFragment);
        textureProgram_ = Program(kQuadVertex, kTextureFragment);
        screenProgram_ = Program(kQuadVertex, kScreenFragment);
        colorProgram_ = Program(kQuadVertex, kColorFragment);
        glGenVertexArrays(1, &vertexArray_);
        glGenTextures(1, &uiTexture_);
        glBindTexture(GL_TEXTURE_2D, uiTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenTextures(1, &screenTexture_);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, screenTexture_);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void Destroy() {
        for (Eye &eye : eyes_) {
            if (eye.length) glDeleteFramebuffers(eye.length, eye.framebuffers);
            if (eye.depth) glDeleteRenderbuffers(1, &eye.depth);
            if (eye.chain) vrapi_DestroyTextureSwapChain(eye.chain);
            eye = Eye{};
        }
        for (GLuint program : {floorProgram_, textureProgram_, screenProgram_, colorProgram_})
            if (program) glDeleteProgram(program);
        floorProgram_ = textureProgram_ = screenProgram_ = colorProgram_ = 0;
        if (vertexArray_) glDeleteVertexArrays(1, &vertexArray_);
        if (uiTexture_) glDeleteTextures(1, &uiTexture_);
        if (screenTexture_) glDeleteTextures(1, &screenTexture_);
        vertexArray_ = uiTexture_ = screenTexture_ = 0;
    }

    bool Ready() const { return eyes_[0].chain != nullptr; }
    GLuint UiTexture() const { return uiTexture_; }
    GLuint ScreenTexture() const { return screenTexture_; }

    void UploadUi(const void *rgba, int width, int height) {
        glBindTexture(GL_TEXTURE_2D, uiTexture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, 0);
        uiLoaded_ = true;
    }

    struct Scene {
        float floorY = 0;
        Quad ui, screen;
        bool screenLive = false;
        float screenTransform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        bool laser = false;
        ovrVector3f laserFrom{}, laserTo{};
        bool cursor = false;
        ovrVector3f cursorAt{};
    };

    ovrLayerProjection2 Render(const ovrTracking2 &tracking, const Scene &scene) {
        ovrLayerProjection2 layer = vrapi_DefaultLayerProjection2();
        layer.HeadPose = tracking.HeadPose;
        for (int eye = 0; eye < 2; eye++) {
            Eye &target = eyes_[eye];
            target.index = (target.index + 1) % target.length;
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffers[target.index]);
            glViewport(0, 0, width_, height_);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, width_, height_);
            glClearColor(0.012f, 0.016f, 0.03f, 1.f);
            glClearDepthf(1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDisable(GL_CULL_FACE);
            glBindVertexArray(vertexArray_);
            ovrMatrix4f viewProjection = ovrMatrix4f_Multiply(&tracking.Eye[eye].ProjectionMatrix, &tracking.Eye[eye].ViewMatrix);

            ovrMatrix4f floor = Model(0, scene.floorY, 0, 40.f, 40.f, true);
            Draw(floorProgram_, viewProjection, floor, nullptr);

            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            if (scene.screen.visible) {
                ovrMatrix4f model = QuadModel(scene.screen);
                if (scene.screenLive) {
                    glUseProgram(screenProgram_);
                    glUniformMatrix4fv(glGetUniformLocation(screenProgram_, "textureMatrix"), 1, GL_FALSE, scene.screenTransform);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, screenTexture_);
                    Draw(screenProgram_, viewProjection, model, nullptr);
                } else {
                    const float dark[4] = {0.02f, 0.02f, 0.025f, 1.f};
                    Draw(colorProgram_, viewProjection, model, dark);
                }
            }
            if (scene.ui.visible && uiLoaded_) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, uiTexture_);
                Draw(textureProgram_, viewProjection, QuadModel(scene.ui), nullptr);
            }
            if (scene.cursor) {
                const float white[4] = {1.f, 1.f, 1.f, 1.f};
                Draw(colorProgram_, viewProjection,
                     Model(scene.cursorAt.x, scene.cursorAt.y, scene.cursorAt.z + 0.004f, 0.018f, 0.018f, false), white);
            }
            if (scene.laser) DrawLaser(viewProjection, scene.laserFrom, scene.laserTo);
            glDisable(GL_BLEND);
            glBindVertexArray(0);
            const GLenum discard = GL_DEPTH_ATTACHMENT;
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discard);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            layer.Textures[eye].ColorSwapChain = target.chain;
            layer.Textures[eye].SwapChainIndex = target.index;
            layer.Textures[eye].TexCoordsFromTanAngles = ovrMatrix4f_TanAngleMatrixFromProjection(&tracking.Eye[eye].ProjectionMatrix);
        }
        return layer;
    }

private:
    struct Eye {
        ovrTextureSwapChain *chain = nullptr;
        int length = 0, index = 0;
        GLuint framebuffers[8]{};
        GLuint depth = 0;
    };
    Eye eyes_[2];
    int width_ = 0, height_ = 0;
    GLuint floorProgram_ = 0, textureProgram_ = 0, screenProgram_ = 0, colorProgram_ = 0;
    GLuint vertexArray_ = 0, uiTexture_ = 0, screenTexture_ = 0;
    bool uiLoaded_ = false;

    /* Quads are drawn from gl_VertexID as the -1..1 square in the XY plane; the floor lies in XZ. */
    static constexpr const char *kQuadVertex =
        "#version 300 es\n"
        "uniform mat4 viewProjection; uniform mat4 model; uniform mat4 textureMatrix;\n"
        "out vec2 uv; out vec3 world;\n"
        "void main(){ vec2 c=vec2(float(gl_VertexID&1),float((gl_VertexID>>1)&1)); vec4 p=model*vec4(c*2.0-1.0,0.0,1.0);\n"
        " world=p.xyz; uv=vec2(c.x,1.0-c.y); gl_Position=viewProjection*p; }\n";
    static constexpr const char *kFloorFragment =
        "#version 300 es\nprecision mediump float; in vec2 uv; in vec3 world; out vec4 color;\n"
        "void main(){ vec2 g=abs(fract(world.xz)-0.5); float line=1.0-smoothstep(0.0,0.02,min(g.x,g.y));\n"
        " float fade=1.0-smoothstep(3.0,18.0,length(world.xz));\n"
        " color=vec4(mix(vec3(0.02,0.025,0.04),vec3(0.10,0.45,0.85),line*fade),1.0); }\n";
    static constexpr const char *kTextureFragment =
        "#version 300 es\nprecision mediump float; in vec2 uv; uniform sampler2D source; out vec4 color;\n"
        "void main(){ color=texture(source,uv); }\n";
    /* SurfaceTexture samples the compositor's sRGB-encoded output as-is; the eye buffers are sRGB, so decode it. */
    static constexpr const char *kScreenFragment =
        "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision mediump float; in vec2 uv; uniform samplerExternalOES source; uniform mat4 textureMatrix; out vec4 color;\n"
        "void main(){ vec2 t=(textureMatrix*vec4(uv.x,1.0-uv.y,0.0,1.0)).xy; vec3 c=texture(source,t).rgb;\n"
        " color=vec4(pow(c,vec3(2.2)),1.0); }\n";
    static constexpr const char *kColorFragment =
        "#version 300 es\nprecision mediump float; uniform vec4 tint; out vec4 color;\n"
        "void main(){ color=tint; }\n";

    static GLuint Shader(GLenum type, const char *text) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glDeleteShader(shader);
            throw std::runtime_error("home shader compile failed");
        }
        return shader;
    }

    static GLuint Program(const char *vertex, const char *fragment) {
        GLuint program = glCreateProgram();
        GLuint v = Shader(GL_VERTEX_SHADER, vertex), f = Shader(GL_FRAGMENT_SHADER, fragment);
        glAttachShader(program, v);
        glAttachShader(program, f);
        glLinkProgram(program);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) throw std::runtime_error("home program link failed");
        return program;
    }

    static ovrMatrix4f Model(float x, float y, float z, float width, float height, bool floor) {
        ovrMatrix4f scale = ovrMatrix4f_CreateScale(width * 0.5f, height * 0.5f, 1.f);
        ovrMatrix4f translation = ovrMatrix4f_CreateTranslation(x, y, z);
        ovrMatrix4f model = ovrMatrix4f_Multiply(&translation, &scale);
        if (!floor) return model;
        ovrMatrix4f flat = ovrMatrix4f_CreateRotation(-(float)M_PI / 2.f, 0.f, 0.f);
        ovrMatrix4f scaled = ovrMatrix4f_Multiply(&flat, &scale);
        return ovrMatrix4f_Multiply(&translation, &scaled);
    }

    static ovrMatrix4f QuadModel(const Quad &quad) { return Model(quad.cx, quad.cy, quad.cz, quad.w, quad.h, false); }

    void Draw(GLuint program, const ovrMatrix4f &viewProjection, const ovrMatrix4f &model, const float *tint) {
        glUseProgram(program);
        glUniformMatrix4fv(glGetUniformLocation(program, "viewProjection"), 1, GL_TRUE, &viewProjection.M[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(program, "model"), 1, GL_TRUE, &model.M[0][0]);
        GLint source = glGetUniformLocation(program, "source");
        if (source >= 0) glUniform1i(source, 0);
        if (tint) glUniform4fv(glGetUniformLocation(program, "tint"), 1, tint);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* A thin camera-independent ribbon: a unit quad stretched along the ray and turned upright. */
    void DrawLaser(const ovrMatrix4f &viewProjection, const ovrVector3f &from, const ovrVector3f &to) {
        const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 1e-3f) return;
        const float yaw = std::atan2(-dx, -dz), pitch = std::asin(dy / length);
        ovrMatrix4f scale = ovrMatrix4f_CreateScale(0.0025f, 0.5f * length, 1.f);
        ovrMatrix4f along = ovrMatrix4f_CreateTranslation(0.f, 0.5f * length, 0.f);
        ovrMatrix4f tilt = ovrMatrix4f_CreateRotation(pitch - (float)M_PI / 2.f, yaw, 0.f);
        ovrMatrix4f place = ovrMatrix4f_CreateTranslation(from.x, from.y, from.z);
        ovrMatrix4f model = ovrMatrix4f_Multiply(&along, &scale);
        model = ovrMatrix4f_Multiply(&tilt, &model);
        model = ovrMatrix4f_Multiply(&place, &model);
        const float blue[4] = {0.25f, 0.6f, 1.f, 0.9f};
        Draw(colorProgram_, viewProjection, model, blue);
    }
};

}
