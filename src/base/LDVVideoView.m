//
//  LDVVideoView.m
//  av1decoder — libdav1d 压测 app
//
//  ★ v8.14：渲染路径换成 OpenGL ES 2.0 直通（GPU 卸载），CPU 路径降为回退。
//
//  为什么要做这一步（iPhone 5 / A6 实测数据）：
//    压测模式（纯解码不渲染）98-125 fps，接到渲染管线后只剩 47-55 fps。
//    旧路径是：CPU 定点转换 YUV→BGRA（LDVColorConvert，-O3）→ 每帧新建
//    CGImage → layer.contents → CoreAnimation 在渲染线程把 **4WH 字节的
//    RGBA** 上传成纹理。854x480@60 就是每秒 ~2460 万像素的逐像素乘加 +
//    ~100 MB/s 的纹理上传，吃掉大半个核；GPU 全程在看热闹。
//
//  新路径（dav1d 出的 planar YUV 原样进 GPU，一个像素都不在 CPU 上转）：
//    Y/U/V 三张 GL_LUMINANCE 纹理（1.5WH 字节，比 RGBA 省 62.5% 上传带宽）
//    → 片元着色器做 YUV→RGB（矩阵 uniform 化，BT.601/709 × video/full
//    range 四套系数与 LDVColorConvert.c 逐值对齐）→ letterbox 按视频宽高比
//    算 quad 顶点 → glDrawArrays → presentRenderbuffer。
//
//  ★ 纹理宽度直接用 **stride**（dav1d 的行距带 128 对齐 padding），把整块
//    平面一次 glTexSubImage2D 上传，靠 uniform 把纹理坐标横向压缩到可见宽
//    度（uYRange = width/stride）。零 CPU 拷贝、零逐行调用 —— ES2 没有
//    GL_UNPACK_ROW_LENGTH，这是不拷贝就能处理 stride 的唯一干净做法。
//    光栅化时像素中心 (x+0.5) 恰好映射回纹素中心 (x+0.5)/stride，连半纹素
//    偏移都没有。
//
//  iOS 4 兼容约束（3GS/iPhone 4 在 iOS 4.x 上也要能跑）：
//    - 不用 CVOpenGLESTextureCache（那是 iOS 5+ 的零拷贝路径，本实现用
//      glTexSubImage2D 直接传 plane 指针）
//    - 不用 GL_RED / GL_RG（iOS 5+ 才有），色度用 GL_LUMINANCE
//    - EAGLContext(kEAGLRenderingAPIOpenGLES2) / CAEAGLLayer 都是 3.0~4.0
//      时代 API，全系可用
//    - SGX535（3GS/4）最大纹理 1024：超限时 per-frame 回退 CPU，不炸
//
//  回退路径（原实现，保留不动）：
//    LDVConvertYUVToBGRA → CGImage → layer.contents。
//    触发条件：bpc > 8（GL 只吃 8bit）、尺寸超 GL 上限、EAGL/着色器初始化
//    失败。bpc/尺寸类回退是**逐帧**的（下一帧 8bit 还回 GL）；GL 真坏了
//    （上下文丢失、链接失败）才永久切回。
//
//  旧版三问的答案仍在（CPU 路径的注释保留）：为什么 3 块轮转缓冲、为什么
//  每帧新建 CGImage —— 那些约束只作用于 CPU 回退路径，GPU 路径没有这些问题
//  （纹理是我们自己管理的 GL 对象，没有 CA 异步读内存这回事）。
//

#import "LDVVideoView.h"

#import "LDVColorConvert.h"

#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>

#if defined(__arm64__) || defined(__aarch64__)
#import "LDVMetalVideoView.h"   /* arm64 双引擎：Metal（iOS8+）优先于 GLES3 */
#endif

/*
 * ★ 头文件显式写全（教训见 v8.13 前的注释：间接包含链随 SDK 漂移）。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* CPU 回退路径的常量（原样保留）                                             */
/* ------------------------------------------------------------------------ */

#define LDV_RGB_BUFFERS 3

/* CGImage 的数据源回调：缓冲区由 view 自己持有并复用，这里什么都不做。 */
static void ldv_provider_release(void *info, const void *data, size_t size) {
    (void)info; (void)data; (void)size;
}

/*
 * 32 位 BGRA 的 CGImage 格式候选表。
 * 只放 "First" 系列（B,G,R,X 内存序）——"Last" 系列会红蓝互换，见旧注释。
 */
static const CGBitmapInfo kLDVBgraBitmapInfos[] = {
    (CGBitmapInfo)(kCGImageAlphaPremultipliedFirst |
                   kCGBitmapByteOrder32Little),
    (CGBitmapInfo)(kCGImageAlphaNoneSkipFirst |
                   kCGBitmapByteOrder32Little),
};
#define LDV_BGRA_FORMAT_COUNT \
    ((int)(sizeof(kLDVBgraBitmapInfos) / sizeof(kLDVBgraBitmapInfos[0])))

/* ------------------------------------------------------------------------ */
/* GLSL 着色器                                                               */
/* ------------------------------------------------------------------------ */

/*
 * 顶点着色器。
 *
 * aPosition 是 clip 坐标（letterbox 已在 CPU 侧算进顶点里）；
 * aTexCoord 是 [0,1] 基准坐标，乘以 uYRange/uCRange 后映射到
 * "宽度 = stride" 的纹理的可见区域。
 */
static const char kLDVVertexShader[] =
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "uniform float uYRange;\n"      /* Y 可见宽 / 纹理宽（= stride0） */
    "uniform float uCRange;\n"      /* C 可见宽 / 纹理宽（= stride1） */
    "varying vec2 vTexY;\n"
    "varying vec2 vTexC;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "    vTexY = vec2(aTexCoord.x * uYRange, aTexCoord.y);\n"
    "    vTexC = vec2(aTexCoord.x * uCRange, aTexCoord.y);\n"
    "}\n";

/*
 * 片元着色器：YUV → RGB。
 *
 * 输入是 0..1 的纹素值，先放大到字节量纲（*255）减去偏移，再乘矩阵，
 * 最后 /255 落回 0..1。全程 mediump：SGX543 的 fp16 精度 ~2^-11，
 * 在 0..255 量纲上误差 < 0.13 字节，人眼不可见（CPU 定点路径本身也有
 * ±1 的舍入）。
 */
static const char kLDVFragmentShader[] =
    "precision mediump float;\n"
    "varying vec2 vTexY;\n"
    "varying vec2 vTexC;\n"
    "uniform sampler2D uTexY;\n"
    "uniform sampler2D uTexU;\n"
    "uniform sampler2D uTexV;\n"
    "uniform mat3 uColorMat;\n"     /* 列0=luma 列1=U 列2=V（列主序） */
    "uniform vec3 uOffsetBytes;\n"  /* (yOff, 128, 128)，字节量纲 */
    "void main() {\n"
    "    vec3 yuv;\n"
    "    yuv.x = texture2D(uTexY, vTexY).r * 255.0 - uOffsetBytes.x;\n"
    "    yuv.y = texture2D(uTexU, vTexC).r * 255.0 - uOffsetBytes.y;\n"
    "    yuv.z = texture2D(uTexV, vTexC).r * 255.0 - uOffsetBytes.z;\n"
    "    vec3 rgb = uColorMat * yuv;\n"
    "    gl_FragColor = vec4(clamp(rgb * (1.0 / 255.0), 0.0, 1.0), 1.0);\n"
    "}\n";

/*
 * 双立方缩放片元着色器 —— 显示端缩小 1.5x 起替代硬件双线性。
 *
 * 核：Keys cubic（a = -0.5），负瓣带来轻锐化（"较锐利"的来源）。
 * 抽头位置随缩放比 step（源纹素/目标像素）展宽，权重是与 step 无关
 * 的常数、行和恰为 1：
 *   4-tap：位置 (k-1.5)·step，权重 {-0.0625, 0.5625, 0.5625, -0.0625}
 *   8-tap：位置 (k-3.5)·step·0.5，
 *          权重 {-0.0117, -0.0352, 0.1133, 0.4336}（镜像）
 *
 * 档位（编译期 TAPMODE 二选一，与 CPU 缩放器分档同源）：
 *   TAPMODE 1：缩小 1.5x~3x —— X/Y 各 4-tap（"8-tap 双立方"，
 *              每平面 16 次采样）
 *   TAPMODE 2：缩小 ≥ 3x —— X 8-tap + Y 4-tap（"12-tap"，
 *              每平面 32 次采样；letterbox 等比缩放下 X/Y 比例相等，
 *              固定取 X 做宽核方向）
 *
 * GLSL ES 1.00 的采样器不能做函数参数，平面采样用宏展开。
 * uStepY/uStepC 是已折算的 uv 步长（含 stride 压缩与色度半分辨率）。
 */
static const char kLDVBicubicFragment[] =
    "precision mediump float;\n"
    "varying vec2 vTexY;\n"
    "varying vec2 vTexC;\n"
    "uniform sampler2D uTexY;\n"
    "uniform sampler2D uTexU;\n"
    "uniform sampler2D uTexV;\n"
    "uniform mat3 uColorMat;\n"
    "uniform vec3 uOffsetBytes;\n"
    "uniform vec2 uStepY;\n"
    "uniform vec2 uStepC;\n"
    "#define LDV_H4(T, UV, ST, DY) ( \\\n"
    "      texture2D(T, UV + vec2(-1.5 * ST.x, DY)).rgb * -0.0625 \\\n"
    "    + texture2D(T, UV + vec2(-0.5 * ST.x, DY)).rgb *  0.5625 \\\n"
    "    + texture2D(T, UV + vec2( 0.5 * ST.x, DY)).rgb *  0.5625 \\\n"
    "    + texture2D(T, UV + vec2( 1.5 * ST.x, DY)).rgb * -0.0625 )\n"
    "#define LDV_H8(T, UV, ST, DY) ( \\\n"
    "      texture2D(T, UV + vec2(-1.75 * ST.x, DY)).rgb * -0.01171875 \\\n"
    "    + texture2D(T, UV + vec2(-1.25 * ST.x, DY)).rgb * -0.03515625 \\\n"
    "    + texture2D(T, UV + vec2(-0.75 * ST.x, DY)).rgb *  0.11328125 \\\n"
    "    + texture2D(T, UV + vec2(-0.25 * ST.x, DY)).rgb *  0.43359375 \\\n"
    "    + texture2D(T, UV + vec2( 0.25 * ST.x, DY)).rgb *  0.43359375 \\\n"
    "    + texture2D(T, UV + vec2( 0.75 * ST.x, DY)).rgb *  0.11328125 \\\n"
    "    + texture2D(T, UV + vec2( 1.25 * ST.x, DY)).rgb * -0.03515625 \\\n"
    "    + texture2D(T, UV + vec2( 1.75 * ST.x, DY)).rgb * -0.01171875 )\n"
    "#if TAPMODE == 1\n"
    "#define LDV_PLANE(T, UV, ST) ( \\\n"
    "      LDV_H4(T, UV, ST, -1.5 * ST.y) * -0.0625 \\\n"
    "    + LDV_H4(T, UV, ST, -0.5 * ST.y) *  0.5625 \\\n"
    "    + LDV_H4(T, UV, ST,  0.5 * ST.y) *  0.5625 \\\n"
    "    + LDV_H4(T, UV, ST,  1.5 * ST.y) * -0.0625 )\n"
    "#else\n"
    "#define LDV_PLANE(T, UV, ST) ( \\\n"
    "      LDV_H8(T, UV, ST, -1.5 * ST.y) * -0.0625 \\\n"
    "    + LDV_H8(T, UV, ST, -0.5 * ST.y) *  0.5625 \\\n"
    "    + LDV_H8(T, UV, ST,  0.5 * ST.y) *  0.5625 \\\n"
    "    + LDV_H8(T, UV, ST,  1.5 * ST.y) * -0.0625 )\n"
    "#endif\n"
    "void main() {\n"
    "    vec3 yuv;\n"
    "    yuv.x = LDV_PLANE(uTexY, vTexY, uStepY).r * 255.0 - uOffsetBytes.x;\n"
    "    yuv.y = LDV_PLANE(uTexU, vTexC, uStepC).r * 255.0 - uOffsetBytes.y;\n"
    "    yuv.z = LDV_PLANE(uTexV, vTexC, uStepC).r * 255.0 - uOffsetBytes.z;\n"
    "    vec3 rgb = uColorMat * yuv;\n"
    "    gl_FragColor = vec4(clamp(rgb * (1.0 / 255.0), 0.0, 1.0), 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------------ */
/* 色彩矩阵（与 LDVColorConvert.c 的 ldv_make_conv 逐值对齐）                 */
/* ------------------------------------------------------------------------ */

/*
 * 列主序 mat3：col0 = luma 向量，col1 = U 系数，col2 = V 系数。
 * rgb255 = M * (Y - yOff, U - 128, V - 128)。
 *
 * video range：luma = 1.164383 * (Y - 16)，色度零点 128；
 * full range：luma = Y，色度零点 128。
 */
static const GLfloat kLDVColorMat601Video[9] = {
    1.164383f, 1.164383f, 1.164383f,
    0.0f, -0.391762f, 2.017232f,
    1.596027f, -0.812968f, 0.0f
};
static const GLfloat kLDVColorMat709Video[9] = {
    1.164383f, 1.164383f, 1.164383f,
    0.0f, -0.213249f, 2.112402f,
    1.792741f, -0.532909f, 0.0f
};
static const GLfloat kLDVColorMat601Full[9] = {
    1.0f, 1.0f, 1.0f,
    0.0f, -0.344136f, 1.772f,
    1.402f, -0.714136f, 0.0f
};
static const GLfloat kLDVColorMat709Full[9] = {
    1.0f, 1.0f, 1.0f,
    0.0f, -0.1873f, 1.8556f,
    1.5748f, -0.4681f, 0.0f
};

/* 与 LDVColorConvert.c 的 ldv_use_bt709 保持一致（含分辨率兜底）。 */
static int ldv_gl_use_bt709(const LDVFrameView *v) {
    switch (v->matrix) {
    case LDV_MATRIX_BT709:      return 1;
    case LDV_MATRIX_BT601:      return 0;
    case LDV_MATRIX_BT470BG:    return 0;
    case LDV_MATRIX_BT2020_NCL: return 1;   /* 2020 借 709 系数（近似） */
    case LDV_MATRIX_BT2020_CL:  return 1;
    default:                    break;
    }
    return v->height > 576;
}

static const GLfloat *ldv_gl_color_matrix(const LDVFrameView *v) {
    const int bt709 = ldv_gl_use_bt709(v);
    if (v->colorRange) return bt709 ? kLDVColorMat709Full : kLDVColorMat601Full;
    return bt709 ? kLDVColorMat709Video : kLDVColorMat601Video;
}

/* ------------------------------------------------------------------------ */
/* LDVGLCanvas — 私有 GL 画布（CAEAGLLayer 承载）                             */
/*                                                                          */
/* 之所以是子视图而不是直接改 layerClass：CPU 回退路径要往**外层**视图的      */
/* layer.contents 画，CAEAGLLayer 一旦换上去就没法干净地退回普通 CALayer。   */
/* 两层互斥：GL 模式画布在最上面且不透明；CPU 模式画布隐藏、外层出图。        */
/* ------------------------------------------------------------------------ */

@interface LDVGLCanvas : UIView
/* 初始化 GL：建上下文、编着色器、建静态几何。失败返回 NO（永回 CPU）。 */
- (BOOL)bootstrap;
/* 本帧能不能走 GL（bpc/布局/尺寸检查，纯查询，无副作用）。 */
- (BOOL)canRenderFrame:(const LDVFrameView *)view;
/* 渲染一帧。返回 NO 表示 GL 运行期失败（调用方应永久切回 CPU）。 */
- (BOOL)drawFrame:(const LDVFrameView *)view;
/* 清成黑屏（对应 CPU 路径的 contents = nil）。 */
- (void)clearToBlack;
/* 真失败标志：上下文/着色器/drawable 级别的不可恢复错误。
 * 瞬时失败（比如还没布局）不算 —— 调用方据此区分"永久切 CPU"
 * 和"本帧跳过、下个 tick 重试"。 */
- (BOOL)isBroken;
/* 实际启用的 GL API 名："GLES3"（iOS7+/A7+ 优先）或 "GLES2"。 */
- (const char *)apiName;
@property (nonatomic, readonly) double lastUploadMilliseconds;
@property (nonatomic, readonly) double lastDrawMilliseconds;
@end

@implementation LDVGLCanvas {
    EAGLContext *_context;

    /* 着色器程序与资源句柄 */
    GLuint _program;
    GLint  _aPos, _aTex;
    GLint  _uYRange, _uCRange, _uMat, _uOffsets;

    /*
     * 双立方档位程序：[0] = TAPMODE 1（8-tap），[1] = TAPMODE 2（12-tap）。
     * 0 = 该档未建成（bootstrap 时静默回退双线性）。
     * ★ 声明别再漏 —— v8.19 真机炸过一整轮 "undeclared identifier"，
     *   方法体和 drawFrame 都写好了、ivar 块没接。
     */
    GLuint _programB[2];
    GLint  _uMatB[2], _uOffB[2], _uYRangeB[2], _uCRangeB[2];
    GLint  _uStepYB[2], _uStepCB[2];
    GLuint _vboPos, _vboTex;         /* 顶点位置 / 纹理坐标（各 4 点） */
    GLuint _texY, _texU, _texV, _texGray;

    /* 帧缓冲 */
    GLuint _fb, _rs;
    GLint  _wantW, _wantH;           /* 期望的 drawable 像素尺寸 */
    GLint  _backingW, _backingH;     /* 实际尺寸（从 renderbuffer 查询） */
    int    _fbReady;

    /* 纹理当前分配的尺寸（变化时才重建，避免每帧重分配） */
    GLsizei _texYW, _texYH, _texUW, _texUH, _texVW, _texVH;

    /* uniform 缓存 key：变化才更新，省每帧 glUniform 调用 */
    int    _matrixKey;               /* matrix<<1 | colorRange */
    int    _rangeKeyW, _rangeKeyCW;  /* 视频宽 / 色度可见宽 */
    int    _rangeS0, _rangeS1;       /* 上次发 range 时的 stride（变化即重发） */
    int    _posKeyVW, _posKeyVH;     /* 视频尺寸（顶点几何的 key 一部分） */

    GLint  _maxTex;

    const char *_apiName;    /* "GLES3" / "GLES2"，实际创建成功的那个 */

    /* 计时（诊断） */
    double _lastUploadMs, _lastDrawMs;
    int    _glBroken;
}

/*
 * ★ 背宿层必须是 CAEAGLLayer —— 用 +layerClass 换。
 *
 * v8.14 首版漏了这个 override：canvas 的 layer 是普通 CALayer，
 * ensureFramebuffer 里 isKindOfClass:[CAEAGLLayer class] 判否，
 * renderbufferStorage:fromDrawable: 根本走不到，第一帧 drawFrame
 * 返回 NO，presentFrame 把它当永久失败，GL 被整个关掉。
 * 而 bootstrap 是成功的，启动日志还打着"直通已启用" —— 日志撒谎。
 * 真机表现：状态栏出现"CPU"前缀，帧率与改造前一模一样。
 * ★ 方法必须在 ivar 块**外面**（写进 { } 里是语法错误，
 *   真机报 "Type name requires a specifier or qualifier"）。
 */
+ (Class)layerClass {
    return [CAEAGLLayer class];
}

- (id)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.opaque = YES;
        /*
         * Retina 设备上 drawable 按 2x 建（iPhone 5 视频区约 626x352 像素，
         * 854 宽的源做 GPU 线性缩小）。UIScreen.scale 是 iOS 4.0+。
         */
        UIScreen *ms = [UIScreen mainScreen];
        if (ms) self.contentScaleFactor = ms.scale;
        _fbReady = 0;
        _glBroken = 0;
        _maxTex = 0;
    }
    return self;
}

- (void)dealloc {
    /* ARC：显式清 GL 对象（需要上下文 current），然后摘掉上下文。 */
    if (_context) {
        if ([EAGLContext setCurrentContext:_context]) {
            if (_fb)     glDeleteFramebuffers(1, &_fb);
            if (_rs)     glDeleteRenderbuffers(1, &_rs);
            if (_texY || _texU || _texV) {
                GLuint tex[3];
                int n = 0;
                if (_texY) tex[n++] = _texY;
                if (_texU) tex[n++] = _texU;
                if (_texV) tex[n++] = _texV;
                glDeleteTextures((GLuint)n, tex);
            }
            if (_texGray) glDeleteTextures(1, &_texGray);
            if (_vboPos || _vboTex) {
                GLuint vb[2];
                int n = 0;
                if (_vboPos) vb[n++] = _vboPos;
                if (_vboTex) vb[n++] = _vboTex;
                glDeleteBuffers((GLuint)n, vb);
            }
            if (_program) glDeleteProgram(_program);
            if (_programB[0]) glDeleteProgram(_programB[0]);
            if (_programB[1]) glDeleteProgram(_programB[1]);
            [EAGLContext setCurrentContext:nil];
        }
    }
}

#pragma mark 着色器与静态资源

static GLuint ldv_gl_compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    if (!sh) return 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        log[0] = '\0';
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log) - 1, &len, log);
        log[len] = '\0';
        fprintf(stderr, "LDVVideo: 着色器编译失败: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

- (BOOL)buildProgram {
    GLuint vs = ldv_gl_compile(GL_VERTEX_SHADER, kLDVVertexShader);
    GLuint fs = 0;
    GLint  linked = 0;

    if (!vs) return NO;
    fs = ldv_gl_compile(GL_FRAGMENT_SHADER, kLDVFragmentShader);
    if (!fs) { glDeleteShader(vs); return NO; }

    _program = glCreateProgram();
    if (!_program) { glDeleteShader(vs); glDeleteShader(fs); return NO; }
    glAttachShader(_program, vs);
    glAttachShader(_program, fs);
    glLinkProgram(_program);
    glDeleteShader(vs);   /* 链接后即可标记删除 */
    glDeleteShader(fs);
    glGetProgramiv(_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        GLsizei len = 0;
        log[0] = '\0';
        glGetProgramInfoLog(_program, (GLsizei)sizeof(log) - 1, &len, log);
        log[len] = '\0';
        fprintf(stderr, "LDVVideo: 着色器链接失败: %s\n", log);
        glDeleteProgram(_program);
        _program = 0;
        return NO;
    }

    _aPos     = glGetAttribLocation(_program, "aPosition");
    _aTex     = glGetAttribLocation(_program, "aTexCoord");
    _uYRange  = glGetUniformLocation(_program, "uYRange");
    _uCRange  = glGetUniformLocation(_program, "uCRange");
    _uMat     = glGetUniformLocation(_program, "uColorMat");
    _uOffsets = glGetUniformLocation(_program, "uOffsetBytes");

    /* 采样器绑到固定纹理单元 0/1/2（一次即可） */
    glUseProgram(_program);
    glUniform1i(glGetUniformLocation(_program, "uTexY"), 0);
    glUniform1i(glGetUniformLocation(_program, "uTexU"), 1);
    glUniform1i(glGetUniformLocation(_program, "uTexV"), 2);

    /*
     * 静态纹理坐标：4 点 TRIANGLE_STRIP，顺序 TL/TR/BL/BR。
     * t=0 是 GL 纹理第 0 行 = 我们数据的第一行 = 画面顶部
     * （先传的是 plane 顶行），所以 TL 的 t=0。
     */
    {
        const GLfloat tex[] = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f };
        glGenBuffers(1, &_vboTex);
        glBindBuffer(GL_ARRAY_BUFFER, _vboTex);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(tex), tex, GL_STATIC_DRAW);
        glGenBuffers(1, &_vboPos);
        glBindBuffer(GL_ARRAY_BUFFER, _vboPos);
        glBufferData(GL_ARRAY_BUFFER, 8 * (GLsizeiptr)sizeof(GLfloat), NULL,
                     GL_DYNAMIC_DRAW);   /* 位置随 letterbox 每次重算 */
    }
    if (_aPos >= 0) {
        glBindBuffer(GL_ARRAY_BUFFER, _vboPos);
        glVertexAttribPointer((GLuint)_aPos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray((GLuint)_aPos);
    }
    if (_aTex >= 0) {
        glBindBuffer(GL_ARRAY_BUFFER, _vboTex);
        glVertexAttribPointer((GLuint)_aTex, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray((GLuint)_aTex);
    }

    /* I400 用：1x1 的 128 灰度纹理，U/V 单元绑它则色度项归零 */
    glGenTextures(1, &_texGray);
    glBindTexture(GL_TEXTURE_2D, _texGray);
    {
        const GLubyte mid = 128;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 1, 1, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, &mid);
    }

    /*
     * ES2 没有像素行对齐的逐行语义兜底，dav1d 的 stride 全是 128 的倍数，
     * 但宽度可能是奇数（如 854 的色度 427）—— UNPACK_ALIGNMENT 统一压到 1，
     * 上传按字节精确对位。
     */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            fprintf(stderr, "LDVVideo: GL 初始化遗留错误 0x%X\n", err);
            return NO;
        }
    }
    return YES;
}

/*
 * 双立方档位程序 ×2。源 = "#define TAPMODE n" 前缀 + 共享片元源。
 * 属性槽位显式绑到双线性程序查询到的 location —— attribute 指针/使能
 * 是全局状态（buildProgram 里设过一次），location 一致则三个程序共用。
 * 任一档失败：对应 _programB[i] = 0，运行期静默回退双线性。
 */
- (BOOL)buildBicPrograms {
    int i;

    for (i = 0; i < 2; i++) {
        char    prefix[32];
        char   *buf;
        size_t  plen, flen;
        GLuint  vs, fs, prog;
        GLint   linked = 0;

        _programB[i] = 0;

        vs = ldv_gl_compile(GL_VERTEX_SHADER, kLDVVertexShader);
        if (!vs) return NO;

        sprintf(prefix, "#define TAPMODE %d\n", i + 1);
        plen = strlen(prefix);
        flen = strlen(kLDVBicubicFragment);
        buf  = (char *)malloc(plen + flen + 1);
        if (!buf) { glDeleteShader(vs); return NO; }
        memcpy(buf, prefix, plen);
        memcpy(buf + plen, kLDVBicubicFragment, flen + 1);

        fs = ldv_gl_compile(GL_FRAGMENT_SHADER, buf);
        free(buf);
        if (!fs) { glDeleteShader(vs); return NO; }

        prog = glCreateProgram();
        if (!prog) { glDeleteShader(vs); glDeleteShader(fs); return NO; }
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        if (_aPos >= 0) glBindAttribLocation(prog, (GLuint)_aPos, "aPosition");
        if (_aTex >= 0) glBindAttribLocation(prog, (GLuint)_aTex, "aTexCoord");
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512];
            GLsizei len = 0;
            log[0] = '\0';
            glGetProgramInfoLog(prog, (GLsizei)sizeof(log) - 1, &len, log);
            log[len] = '\0';
            fprintf(stderr, "LDVVideo: 双立方程序 %d 链接失败: %s\n", i + 1, log);
            glDeleteProgram(prog);
            return NO;
        }

        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "uTexY"), 0);
        glUniform1i(glGetUniformLocation(prog, "uTexU"), 1);
        glUniform1i(glGetUniformLocation(prog, "uTexV"), 2);

        _programB[i]  = prog;
        _uMatB[i]     = glGetUniformLocation(prog, "uColorMat");
        _uOffB[i]     = glGetUniformLocation(prog, "uOffsetBytes");
        _uYRangeB[i]  = glGetUniformLocation(prog, "uYRange");
        _uCRangeB[i]  = glGetUniformLocation(prog, "uCRange");
        _uStepYB[i]   = glGetUniformLocation(prog, "uStepY");
        _uStepCB[i]   = glGetUniformLocation(prog, "uStepC");
    }
    return YES;
}

- (BOOL)bootstrap {
    /*
     * ES3 优先（arm64 全系支持；SGX543 老 GPU 会创建失败 → 回退 ES2）。
     * ES3 context 完全兼容本类的 ES2 API 管线（GL_LUMINANCE 是 ES3
     * 保留的未定尺寸格式，GLSL 100 着色器照常编译）—— 先拿 context
     * 升级，texelFetch/UBO 等 shader 层升级是下一步的事。
     */
    _context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    _apiName = "GLES3";
    if (!_context) {
        _context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
        _apiName = "GLES2";
    }
    if (!_context) {
        fprintf(stderr, "LDVVideo: EAGLContext(ES3/ES2) 创建失败，走 CPU 回退\n");
        return NO;
    }
    if (![EAGLContext setCurrentContext:_context]) return NO;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_maxTex);
    if (![self buildProgram]) return NO;
    /* 双立方档位构建失败不致命：对应档位静默回退双线性 */
    if (![self buildBicPrograms]) {
        fprintf(stderr, "LDVVideo: 双立方档位构建失败，缩放采样回退双线性\n");
    }
    [EAGLContext setCurrentContext:nil];
    return YES;
    /* 中途失败时已建的部分对象随 context 释放在 dealloc 里清理 */
}

#pragma mark 帧缓冲

- (BOOL)ensureFramebuffer {
    CAEAGLLayer *l = (CAEAGLLayer *)self.layer;
    GLint wantW, wantH;

    if (![l isKindOfClass:[CAEAGLLayer class]]) return NO;

    wantW = (GLint)(l.bounds.size.width  * l.contentsScale + 0.5);
    wantH = (GLint)(l.bounds.size.height * l.contentsScale + 0.5);
    if (wantW <= 0 || wantH <= 0) return NO;   /* 还没布局：不算失败 */

    if (_fbReady && wantW == _wantW && wantH == _wantH) return YES;

    if (!_fb) {
        glGenFramebuffers(1, &_fb);
        glGenRenderbuffers(1, &_rs);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, _fb);
    glBindRenderbuffer(GL_RENDERBUFFER, _rs);

    /*
     * renderbufferStorage:fromDrawable: 是 CAEAGLLayer 的正规建法：
     * 尺寸由图层 bounds x contentsScale 决定，格式取自 drawableProperties。
     * bounds 变化（旋转/布局）后重调即可完成 resize。
     */
    if (![_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:l]) {
        fprintf(stderr, "LDVVideo: drawable 分配失败（%dx%d）\n", wantW, wantH);
        _glBroken = 1;   /* 真失败：不设标志会变成每帧静默重试、永远黑屏 */
        return NO;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, _rs);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "LDVVideo: framebuffer 不完整\n");
        _glBroken = 1;
        return NO;
    }
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH,
                                 &_backingW);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT,
                                 &_backingH);
    _wantW = wantW;
    _wantH = wantH;
    _fbReady = 1;
    _posKeyVW = 0;   /* 几何依赖 backing 尺寸，标记重算 */
    _posKeyVH = 0;
    return YES;
}

- (void)setDrawablePropertiesIfNeeded {
    CAEAGLLayer *l = (CAEAGLLayer *)self.layer;
    if (![l isKindOfClass:[CAEAGLLayer class]]) return;
    if ([l.drawableProperties count] > 0) return;   /* 已设置 */
    /*
     * iOS 4 可用的字典构造（不用 @{} 字面量，兼容老运行时的写法纪律）。
     * retainedBacking=NO：每帧全量重绘（我们本来就 clear+全画），
     * 换 NO 能省一份后备缓冲内存。
     */
    l.drawableProperties = [NSDictionary
        dictionaryWithObjectsAndKeys:
            [NSNumber numberWithBool:NO],
                kEAGLDrawablePropertyRetainedBacking,
            kEAGLColorFormatRGBA8,
                kEAGLDrawablePropertyColorFormat,
            nil];
}

#pragma mark 纹理

/* 按布局返回色度平面尺寸；I400 返回 NO（无需色度纹理）。 */
static int ldv_gl_chroma_dims(const LDVFrameView *v,
                              int *cw, int *ch) {
    switch (v->layout) {
    case LDV_PIXEL_LAYOUT_I420: *cw = (v->width + 1) / 2; *ch = (v->height + 1) / 2; return 1;
    case LDV_PIXEL_LAYOUT_I422: *cw = (v->width + 1) / 2; *ch = v->height; return 1;
    case LDV_PIXEL_LAYOUT_I444: *cw = v->width; *ch = v->height; return 1;
    default: return 0;
    }
}

static void ldv_gl_tex_params(void) {
    /* NPOT（854/896 之类）硬性要求：CLAMP_TO_EDGE + 无 mipmap 过滤 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/*
 * 上传一个平面。宽用 stride（整行连续，一次调用传完），高用可见行数。
 * 尺寸没变走 TexSubImage，变了走 TexImage（重新分配）。
 */
static BOOL ldv_gl_upload_plane(GLuint *tex, GLsizei *tw, GLsizei *th,
                                GLsizei w, GLsizei h,
                                const uint8_t *data) {
    glBindTexture(GL_TEXTURE_2D, *tex);
    if (!*tex) return NO;
    if (*tw != w || *th != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
        ldv_gl_tex_params();
        *tw = w;
        *th = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
    }
    return YES;
}

- (BOOL)ensureTextures:(const LDVFrameView *)view {
    int cw = 0, ch = 0;
    const GLsizei yW = (GLsizei)view->stride[0];
    const GLsizei yH = (GLsizei)view->height;
    const GLsizei cW = view->layout == LDV_PIXEL_LAYOUT_I400
                           ? 0 : (GLsizei)view->stride[1];
    int hasChroma = ldv_gl_chroma_dims(view, &cw, &ch);

    if (!_texY) {
        glGenTextures(1, &_texY);
        glGenTextures(1, &_texU);
        glGenTextures(1, &_texV);
        _texYW = _texYH = _texUW = _texUH = _texVW = _texVH = 0;
    }

    if (!ldv_gl_upload_plane(&_texY, &_texYW, &_texYH, yW, yH,
                             view->plane[0])) return NO;

    if (hasChroma) {
        if (!ldv_gl_upload_plane(&_texU, &_texUW, &_texUH, cW, (GLsizei)ch,
                                 view->plane[1])) return NO;
        if (!ldv_gl_upload_plane(&_texV, &_texVW, &_texVH, cW, (GLsizei)ch,
                                 view->plane[2])) return NO;
    }
    return YES;
}

#pragma mark 渲染

- (BOOL)canRenderFrame:(const LDVFrameView *)view {
    int cw = 0, ch = 0;

    if (_glBroken || !_program || _maxTex <= 0) return NO;
    if (view->bpc != 8) return NO;              /* GL 只吃 8bit，高位深回 CPU */
    if (!view->plane[0] || view->width <= 0 || view->height <= 0) return NO;
    if (view->stride[0] < view->width) return NO;
    if (view->stride[0] > _maxTex || view->height > _maxTex) {
        return NO;                              /* SGX535 只有 1024 */
    }
    if (view->layout == LDV_PIXEL_LAYOUT_I400) return YES;
    if (!ldv_gl_chroma_dims(view, &cw, &ch)) return NO;
    if (!view->plane[1] || !view->plane[2]) return NO;
    if (view->stride[1] < cw) return NO;
    if (view->stride[1] > _maxTex || ch > _maxTex) return NO;
    return YES;
}

/* letterbox：视频按宽高比贴合进 backing，居中，clip 坐标写入 VBO */
- (void)updateGeometryVideoW:(int)vw h:(int)vh {
    GLfloat pos[8];
    double  bw = (double)_backingW, bh = (double)_backingH;
    double  scale = (bw / (double)vw < bh / (double)vh)
                        ? bw / (double)vw : bh / (double)vh;
    double  dw = (double)vw * scale, dh = (double)vh * scale;
    double  px0 = (bw - dw) / 2.0, py0 = (bh - dh) / 2.0;
    double  fx0, fx1, fyTop, fyBot;

    if (dw < 1.0) dw = 1.0;
    if (dh < 1.0) dh = 1.0;
    fx0   = -1.0 + 2.0 * px0 / bw;
    fx1   = -1.0 + 2.0 * (px0 + dw) / bw;
    fyTop = 1.0 - 2.0 * py0 / bh;
    fyBot = 1.0 - 2.0 * (py0 + dh) / bh;

    pos[0] = (GLfloat)fx0;   pos[1] = (GLfloat)fyTop;   /* TL */
    pos[2] = (GLfloat)fx1;   pos[3] = (GLfloat)fyTop;   /* TR */
    pos[4] = (GLfloat)fx0;   pos[5] = (GLfloat)fyBot;   /* BL */
    pos[6] = (GLfloat)fx1;   pos[7] = (GLfloat)fyBot;   /* BR */

    glBindBuffer(GL_ARRAY_BUFFER, _vboPos);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)sizeof(pos), pos);
    _posKeyVW = vw;
    _posKeyVH = vh;
}

- (BOOL)drawFrame:(const LDVFrameView *)view {
    const GLfloat *mat;
    GLfloat        offsets[3];
    int            cw = 0, ch = 0, hasChroma;
    int            mkey;
    int            mode;            /* 0 双线性 / 1 8-tap / 2 12-tap */
    GLuint         prog;
    double         t0, t1, t2;

    if (_glBroken) return NO;
    if (![EAGLContext setCurrentContext:_context]) { _glBroken = 1; return NO; }

    [self setDrawablePropertiesIfNeeded];
    if (![self ensureFramebuffer]) {
        if (_wantW > 0) { _glBroken = 1; return NO; }  /* 真失败 */
        return NO;                                     /* 还没布局 */
    }

    t0 = CACurrentMediaTime();

    hasChroma = ldv_gl_chroma_dims(view, &cw, &ch);
    if (![self ensureTextures:view]) { _glBroken = 1; return NO; }

    /*
     * ---- 采样档位：显示端缩放比 ----
     *
     * letterbox 等比缩放，r = 1/scale = max(vw/bw, vh/bh)：
     *   r < 1.5          → 硬件双线性（最便宜；窗口贴合预缩后 r≈1，
     *                      常态都落在这档）
     *   1.5 ≤ r < 3      → 8-tap 双立方（X/Y 各 4-tap cubic）
     *   r ≥ 3            → 12-tap（X 8-tap + Y 4-tap）
     * 档位程序没建成功就降回双线性。
     */
    {
        const double bw2 = (double)_backingW, bh2 = (double)_backingH;
        const double sc  = (bw2 / (double)view->width < bh2 / (double)view->height)
                               ? bw2 / (double)view->width
                               : bh2 / (double)view->height;
        const double r = (sc > 0.0) ? 1.0 / sc : 1.0;
        mode = (r >= 3.0) ? 2 : ((r >= 1.5) ? 1 : 0);
        if (mode != 0 && !_programB[mode - 1]) mode = 0;
        /* v8.22：双立方只给 arm64（GLES3；Metal 走自己的 ldvBicubic-
         * Plane）。armv7 ES2 定案二次线性 —— SGX543 上 8-tap 是
         * 16 fetch/平面的可感知负担，文档原案就是硬件双线性。
         * （GLES3 ⟺ A7+/arm64；ES2 档位程序虽建出来但不选用。） */
        if (mode != 0 && 0 != strcmp(_apiName, "GLES3")) mode = 0;
    }
    prog = (mode == 0) ? _program : _programB[mode - 1];

    /* ---- uniforms ---- */
    if (mode != 0) {
        /*
         * 双立方档：触发频率低（缓存未预缩/换挡场景），不做 key 缓存，
         * 每帧全量推。步长 = 每目标像素的源纹素数，折算成 uv
         * （除以纹理宽 = stride；垂直方向纹理高 = 帧高，步长 = 1/dh）。
         */
        const double bw2 = (double)_backingW, bh2 = (double)_backingH;
        const double sc  = (bw2 / (double)view->width < bh2 / (double)view->height)
                               ? bw2 / (double)view->width
                               : bh2 / (double)view->height;
        const double dw = (double)view->width * sc;
        const double dh = (double)view->height * sc;
        const int    bi = mode - 1;

        glUseProgram(prog);
        glUniformMatrix3fv(_uMatB[bi], 1, GL_FALSE, ldv_gl_color_matrix(view));
        offsets[0] = view->colorRange ? 0.0f : 16.0f;
        offsets[1] = 128.0f;
        offsets[2] = 128.0f;
        glUniform3fv(_uOffB[bi], 1, offsets);
        glUniform1f(_uYRangeB[bi], (GLfloat)view->width / (GLfloat)view->stride[0]);
        glUniform1f(_uCRangeB[bi],
                    hasChroma ? (GLfloat)cw / (GLfloat)view->stride[1] : 1.0f);
        glUniform2f(_uStepYB[bi],
                    (GLfloat)((double)view->width / dw / (double)view->stride[0]),
                    (GLfloat)(1.0 / dh));
        glUniform2f(_uStepCB[bi],
                    hasChroma
                        ? (GLfloat)((double)cw / dw / (double)view->stride[1])
                        : 0.0f,
                    hasChroma ? (GLfloat)(1.0 / dh) : 0.0f);

        /*
         * 失效双线性程序的 key：uniform 状态按程序隔离，切回 mode 0
         * 时若 key 未失效会漏发 —— 用哨兵值强制全量重发。
         */
        _matrixKey = -1;
        _rangeKeyW = -1;
        _rangeKeyCW = -1;
        _rangeS0 = -1;
        _rangeS1 = -1;
    } else {
        /* ---- 双线性：矩阵 / 偏移（key 变了才发） ---- */
        mkey = ((int)view->matrix << 1) | (view->colorRange ? 1 : 0);
        if (mkey != _matrixKey) {
            mat = ldv_gl_color_matrix(view);
            offsets[0] = view->colorRange ? 0.0f : 16.0f;
            offsets[1] = 128.0f;
            offsets[2] = 128.0f;
            glUseProgram(_program);
            glUniformMatrix3fv(_uMat, 1, GL_FALSE, mat);
            glUniform3fv(_uOffsets, 1, offsets);
            glUniform1f(_uYRange, (GLfloat)view->width / (GLfloat)view->stride[0]);
            glUniform1f(_uCRange,
                        hasChroma ? (GLfloat)cw / (GLfloat)view->stride[1] : 1.0f);
            _matrixKey = mkey;
            _rangeKeyW = 0;   /* range 依赖布局，标记重算 */
            _rangeKeyCW = 0;
            _rangeS0 = 0;
            _rangeS1 = 0;
        } else if (_rangeKeyW != (int)view->width ||
                   _rangeKeyCW != cw ||
                   _rangeS0 != (int)view->stride[0] ||
                   _rangeS1 != (int)view->stride[1]) {
            /* 矩阵没变但尺寸/stride 变了（罕见：中途换分辨率或对齐漂移） */
            glUseProgram(_program);
            glUniform1f(_uYRange, (GLfloat)view->width / (GLfloat)view->stride[0]);
            glUniform1f(_uCRange,
                        hasChroma ? (GLfloat)cw / (GLfloat)view->stride[1] : 1.0f);
            _rangeKeyW = (int)view->width;
            _rangeKeyCW = cw;
            _rangeS0 = (int)view->stride[0];
            _rangeS1 = (int)view->stride[1];
        }
    }

    /* ---- 几何：视频尺寸或 backing 尺寸变了才重算 letterbox ---- */
    if (_posKeyVW != view->width || _posKeyVH != view->height) {
        [self updateGeometryVideoW:view->width h:view->height];
    }

    /* ---- 纹理绑定（每帧 3 次 bind，保证 I400 的灰度兜底正确） ---- */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _texY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, hasChroma ? _texU : _texGray);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, hasChroma ? _texV : _texGray);

    /* ---- 绘制与提交 ---- */
    glUseProgram(prog);
    glViewport(0, 0, _backingW, _backingH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    t1 = CACurrentMediaTime();

    glBindRenderbuffer(GL_RENDERBUFFER, _rs);
    if (![_context presentRenderbuffer:GL_RENDERBUFFER]) {
        _glBroken = 1;
        return NO;
    }
    t2 = CACurrentMediaTime();

    _lastUploadMs = (t1 - t0) * 1000.0;   /* 上传 + uniform + 命令提交 */
    _lastDrawMs   = (t2 - t1) * 1000.0;   /* present（含合成器交接） */
    return YES;
}

- (void)clearToBlack {
    if (_glBroken) return;
    if (![EAGLContext setCurrentContext:_context]) return;
    if (![self ensureFramebuffer]) return;
    glViewport(0, 0, _backingW, _backingH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindRenderbuffer(GL_RENDERBUFFER, _rs);
    [_context presentRenderbuffer:GL_RENDERBUFFER];
}

- (BOOL)isBroken { return _glBroken != 0; }
- (const char *)apiName { return _apiName; }
- (double)lastUploadMilliseconds { return _lastUploadMs; }
- (double)lastDrawMilliseconds   { return _lastDrawMs; }

@end

/* ------------------------------------------------------------------------ */
/* LDVVideoView — 对外接口不变，内部 GL 优先、CPU 回退                        */
/* ------------------------------------------------------------------------ */

@interface LDVVideoView ()
@property (nonatomic, assign) CGImageRef currentImage;
@end

@implementation LDVVideoView {
    /* ---- GPU 直通（引擎选择一次：Metal > GLES3 > GLES2 > CPU） ---- */
    LDVGLCanvas *_canvas;
    int          _glMode;          /* GL 可用且未坏；坏了永久回退 CPU */
    int          _glTried;         /* GL bootstrap 只试一次 */
    int          _glFailStreak;    /* GL 连续瞬时失败计数 → 30 帧判真失败 */
#if defined(__arm64__) || defined(__aarch64__)
    LDVMetalCanvas *_metalCanvas;
    int             _metalMode;   /* Metal（arm64 + iOS8+）可用且未坏 */
    int             _metalFailStreak; /* 同上：Metal 的连续失败看门狗 */
#endif

    /* ---- CPU 回退路径（原实现保留） ---- */
    uint8_t         *_rgb[LDV_RGB_BUFFERS];
    size_t           _rgbCapacity[LDV_RGB_BUFFERS];
    CGImageRef       _img[LDV_RGB_BUFFERS];
    int              _rgbWrite;        /* 下一个要写的块 */
    int              _rgbShown;        /* 正在被图层显示的那块（-1 = 无） */

    size_t           _rgbStride;
    int              _rgbWidth;
    int              _rgbHeight;
    int64_t          _presented;
    int              _imageFormatFailed;  /* 候选格式全失败时置位，避免每帧刷日志 */
    double           _lastConvertMs;      /* 最近一次色彩转换耗时（诊断用） */

    /* 两种路径共用的"最后显示尺寸"（状态栏的"画面 WxH"读它） */
    int              _lastW;
    int              _lastH;
}

- (id)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.opaque = YES;
        /* CPU 回退路径让图层自己按比例缩放并留黑边 */
        self.layer.contentsGravity = kCAGravityResizeAspect;
        self.layer.magnificationFilter = kCAFilterLinear;
        self.layer.minificationFilter = kCAFilterLinear;
        _presented = 0;
        _rgbWrite = 0;
        _rgbShown = -1;
        _glMode = 0;
        _glTried = 0;

        /*
         * GL 画布：先创建但**延迟 bootstrap** —— Metal 激活时不需要
         * 第二个 GL 上下文；Metal 中途坏了再由 ensureGLCanvas 补建。
         */
        {
            LDVGLCanvas *c = [[LDVGLCanvas alloc] initWithFrame:self.bounds];
            c.autoresizingMask =
                UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            c.hidden = YES;
            [self addSubview:c];
            _canvas = c;
        }

#if defined(__arm64__) || defined(__aarch64__)
        /*
         * 引擎选择（第 7 代，只判一次）：arm64 + iOS 8+ 且 Metal device
         * 可用 → Metal；否则（arm64 iOS7 走 GLES3，armv7 走 GLES2）。
         */
        if ([LDVMetalCanvas isSupported]) {
            LDVMetalCanvas *m = [[LDVMetalCanvas alloc] initWithFrame:self.bounds];
            m.autoresizingMask =
                UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            m.hidden = YES;
            [self addSubview:m];
            if ([m bootstrap]) {
                _metalCanvas = m;
                _metalMode = 1;
                m.hidden = NO;
                fprintf(stderr, "LDVVideo: Metal 渲染已启用"
                                "（arm64，YUV buffer 三套轮转，零重建纹理）\n");
            } else {
                fprintf(stderr, "LDVMetal: 初始化失败，降级 GLES\n");
                [m removeFromSuperview];
            }
        } else {
            fprintf(stderr, "LDVMetal: 运行时不支持（iOS<8 / 无 device），走 GLES\n");
        }
#endif

        [self ensureGLCanvas];
    }
    return self;
}

/*
 * GL 画布懒启动：Metal 激活时不建第二个上下文；Metal 中途坏了才补。
 * 返回 YES 表示 GL 画布处于可用状态（_glMode=1）。
 *
 * ★ _glTried 只在**真的尝试过** bootstrap 后才置位 —— v8.19 首版把
 *   `_glTried = 1` 放在 Metal 存活检查之前，Metal 活着时这次"没试的
 *   尝试"就把唯一机会烧掉了，Metal 运行期坏掉后 GL 永远建不起来，
 *   整条降级链断在第一环。
 */
- (BOOL)ensureGLCanvas {
#if defined(__arm64__) || defined(__aarch64__)
    if (_metalMode) return NO;   /* Metal 活着，GL 留作降级备胎（不烧尝试机会） */
#endif
    if (_glTried) return _glMode != 0;
    _glTried = 1;
    if ([_canvas bootstrap]) {
        _glMode = 1;
        _canvas.hidden = NO;
        fprintf(stderr, "LDVVideo: %s 直通已启用"
                        "（YUV 三纹理 + 着色器，CPU 不再做逐像素转换）\n",
                [_canvas apiName]);
    } else {
        fprintf(stderr, "LDVVideo: GLES 不可用，走 CPU 转换回退\n");
    }
    return _glMode != 0;
}

- (void)dealloc {
    int i;
    for (i = 0; i < LDV_RGB_BUFFERS; i++) {
        if (_img[i]) { CGImageRelease(_img[i]); _img[i] = NULL; }
        if (_rgb[i]) { free(_rgb[i]); _rgb[i] = NULL; }
    }
    /* _canvas 由 ARC 释放，GL 对象在它的 dealloc 里清 */
}

- (CGSize)currentVideoSize {
    if (_lastW <= 0 || _lastH <= 0) return CGSizeZero;
    return CGSizeMake((CGFloat)_lastW, (CGFloat)_lastH);
}

- (int64_t)presentedFrameCount {
    return _presented;
}

/*
 * 最近一次"呈现一帧"的耗时（毫秒）：
 *   GPU 模式（Metal/GLES）= 纹理上传 + uniform + present；
 *   CPU 模式 = 逐像素转换。
 * 60 fps 预算 16.7 ms，读数超了就是渲染端成了瓶颈。
 */
- (double)lastConvertMilliseconds {
#if defined(__arm64__) || defined(__aarch64__)
    if (_metalMode) {
        return _metalCanvas.lastUploadMilliseconds +
               _metalCanvas.lastDrawMilliseconds;
    }
#endif
    if (_glMode) {
        return _canvas.lastUploadMilliseconds + _canvas.lastDrawMilliseconds;
    }
    return _lastConvertMs;
}

- (const char *)renderBackendName {
#if defined(__arm64__) || defined(__aarch64__)
    if (_metalMode) return "Metal";
#endif
    if (_glMode) return [_canvas apiName];
    return "CPU";
}

/* 释放 CPU 回退路径的全部缓冲。必须先断开图层引用再 free。 */
- (void)releaseBuffers {
    int i;
    self.layer.contents = nil;
    for (i = 0; i < LDV_RGB_BUFFERS; i++) {
        if (_img[i]) { CGImageRelease(_img[i]); _img[i] = NULL; }
        if (_rgb[i]) { free(_rgb[i]); _rgb[i] = NULL; }
        _rgbCapacity[i] = 0;
    }
    _rgbShown = -1;
    _rgbWrite = 0;
}

- (void)presentFrame:(const LDVFrameView *)view {
    const int    w = view ? view->width  : 0;
    const int    h = view ? view->height : 0;

    if (!view || w <= 0 || h <= 0) {
#if defined(__arm64__) || defined(__aarch64__)
        if (_metalMode) [_metalCanvas clearToBlack];
        else
#endif
        if (_glMode) [_canvas clearToBlack];
        else         self.layer.contents = nil;
        return;
    }

    _lastW = w;
    _lastH = h;

#if defined(__arm64__) || defined(__aarch64__)
    /* ---- Metal 直通（arm64 引擎首选） ---- */
    if (_metalMode && [_metalCanvas canRenderFrame:view]) {
        if ([_metalCanvas drawFrame:view]) {
            _metalFailStreak = 0;
            _presented++;
            if (_presented == 1) {
                fprintf(stderr,
                        "LDVVideo: Metal 首帧已提交 %dx%d %s %s"
                        "（backend %.0fx%.0f 像素，上传 %.2f ms"
                        " + present %.2f ms）\n",
                        w, h,
                        view->layout == LDV_PIXEL_LAYOUT_I420 ? "I420" :
                        (view->layout == LDV_PIXEL_LAYOUT_I422 ? "I422" :
                         (view->layout == LDV_PIXEL_LAYOUT_I444 ? "I444" : "I400")),
                        LDVColorConvertMatrixName(view),
                        (double)_metalCanvas.bounds.size.width *
                            _metalCanvas.contentScaleFactor,
                        (double)_metalCanvas.bounds.size.height *
                            _metalCanvas.contentScaleFactor,
                        _metalCanvas.lastUploadMilliseconds,
                        _metalCanvas.lastDrawMilliseconds);
            }
            return;
        }
        if (_metalCanvas.isBroken || ++_metalFailStreak >= 30) {
            /*
             * Metal 真失败：降级 GLES（ensureGLCanvas 此时才建 GL 上下文），
             * 与 GLES 的降级规则一致 —— 瞬时失败只跳帧，真失败才换引擎。
             * ★ 连续 30 帧（0.5 秒）也算真失败：看门狗防"每帧都瞬时失败"
             *   的死循环 —— 那种失败永远不置 broken 标志，旧逻辑下就是
             *   永久黑屏 + 解码照跑 + CPU 空转（SE1 实测症状）。
             */
            fprintf(stderr, "LDVVideo: Metal 运行期失败（streak %d），降级 GLES\n",
                    _metalFailStreak);
            _metalFailStreak = 0;
            _metalMode = 0;
            _metalCanvas.hidden = YES;
            [self ensureGLCanvas];
        }
        /* 无论哪种失败，本帧都跳过（同 GL 路径的瞬时/真失败语义） */
        return;
    }
#endif

    /* ---- GLES 直通 ---- */
    if (_glMode && [_canvas canRenderFrame:view]) {
        if ([_canvas drawFrame:view]) {
            _glFailStreak = 0;
            _presented++;
            if (_presented == 1) {
                fprintf(stderr,
                        "LDVVideo: GL 首帧已提交 %dx%d %s %s"
                        "（backend %.0fx%.0f 像素，上传 %.2f ms"
                        " + present %.2f ms）\n",
                        w, h,
                        view->layout == LDV_PIXEL_LAYOUT_I420 ? "I420" :
                        (view->layout == LDV_PIXEL_LAYOUT_I422 ? "I422" :
                         (view->layout == LDV_PIXEL_LAYOUT_I444 ? "I444" : "I400")),
                        LDVColorConvertMatrixName(view),
                        (double)_canvas.bounds.size.width * _canvas.contentScaleFactor,
                        (double)_canvas.bounds.size.height * _canvas.contentScaleFactor,
                        _canvas.lastUploadMilliseconds,
                        _canvas.lastDrawMilliseconds);
            }
            return;
        }
        /*
         * ★ 只在**真失败**（上下文丢失 / present 失败 / drawable 建不起来）
         * 时才永久切回 CPU。瞬时失败（如布局还没跑完、bounds 为 0）只跳过
         * 本帧，下个 tick 重试 —— v8.14 首版把两者混在一起，第一帧的瞬时
         * NO 就把 GL 整个关了，表现是状态栏一直显示 CPU。
         */
        if (_canvas.isBroken || ++_glFailStreak >= 30) {
            /*
             * 真失败或连续 30 帧瞬时失败（看门狗，同 Metal 路径）——
             * 永久切回 CPU。瞬时失败（如布局还没跑完、bounds 为 0）
             * 只跳过本帧，下个 tick 重试。
             */
            fprintf(stderr, "LDVVideo: GLES 运行期失败（streak %d），"
                            "永久切回 CPU 回退\n", _glFailStreak);
            _glFailStreak = 0;
            _glMode = 0;
            _canvas.hidden = YES;
        }
        /* 无论哪种失败，本帧都跳过：真失败下个 tick 走 CPU 路径，
         * 瞬时失败下个 tick 重试 GL。最多丢一帧显示，不闪黑。 */
        return;
    }
    /* bpc/尺寸类不满足 canRenderFrame：本帧走 CPU，不禁用 GPU */

    /* ---- CPU 回退路径（原实现原样保留） ---- */
    {
        size_t       stride;
        size_t       need;

        stride = (size_t)w * 4;
        need   = stride * (size_t)h;

        /*
         * 尺寸变化时重建全部缓冲（正常只发生一次，即第一帧）。
         *
         * ★ 顺序很重要：必须先断开图层对旧 CGImage 的引用，再释放旧缓冲。
         *   CGImage 的数据源直接指向 _rgb，而 CoreAnimation 是**异步**读取
         *   图层内容的 —— 先 free 会读到已释放的内存。
         */
        if (!_rgb[0] || _rgbWidth != w || _rgbHeight != h ||
            _rgbCapacity[0] < need) {
            int i;
            [self releaseBuffers];
            for (i = 0; i < LDV_RGB_BUFFERS; i++) {
                _rgb[i] = (uint8_t *)malloc(need);
                if (!_rgb[i]) {
                    fprintf(stderr, "LDVVideo: BGRA 缓冲分配失败"
                                    "（第 %d 块，需要 %zu 字节）\n", i, need);
                    [self releaseBuffers];
                    return;
                }
                _rgbCapacity[i] = need;
            }
            _rgbStride = stride;
            _rgbWidth = w;
            _rgbHeight = h;
            _imageFormatFailed = 0;   /* 新尺寸：给候选格式一次重试机会 */
        }

        /*
         * 选一块**不是正在显示**的缓冲来写。
         * 3 块轮转下这一步天然成立，这里是防御性写法。
         */
        {
            int slot = _rgbWrite;
            _rgbWrite = (_rgbWrite + 1) % LDV_RGB_BUFFERS;
            if (slot == _rgbShown) {
                slot = _rgbWrite;
                _rgbWrite = (_rgbWrite + 1) % LDV_RGB_BUFFERS;
            }

            /* ---- 色彩转换（纯 C，单独 -O3 编译）---- */
            {
                const double t0 = CACurrentMediaTime();
                LDVConvertYUVToBGRA(view, _rgb[slot], _rgbStride);
                _lastConvertMs = (CACurrentMediaTime() - t0) * 1000.0;
            }

            /*
             * 每帧新建一个 CGImage 对象（但**不拷贝像素**）：
             *   - 数据源直接指向 _rgb[slot]，没有 memcpy
             *   - 必须是新对象，否则 CoreAnimation 不会重新上传纹理
             *     （它按对象标识缓存）
             */
            {
                CGDataProviderRef provider =
                    CGDataProviderCreateWithData(NULL, _rgb[slot], need,
                                                 ldv_provider_release);
                CGColorSpaceRef cs;
                CGImageRef      img = NULL;
                int             fi;

                if (!provider) {
                    fprintf(stderr, "LDVVideo: CGDataProvider 创建失败\n");
                    return;
                }

                cs = CGColorSpaceCreateDeviceRGB();

                for (fi = 0; fi < LDV_BGRA_FORMAT_COUNT && !img; fi++) {
                    img = CGImageCreate((size_t)w, (size_t)h,
                                        8, 32, _rgbStride, cs,
                                        kLDVBgraBitmapInfos[fi],
                                        provider, NULL, false,
                                        kCGRenderingIntentDefault);
                    if (!img && fi + 1 < LDV_BGRA_FORMAT_COUNT) {
                        fprintf(stderr,
                                "LDVVideo: CGImageCreate 不接受 BGRX 格式 #%d"
                                "（bitmapInfo=0x%X），换下一种\n",
                                fi, (unsigned)kLDVBgraBitmapInfos[fi]);
                    }
                }

                CGColorSpaceRelease(cs);
                CGDataProviderRelease(provider);

                if (!img) {
                    if (!_imageFormatFailed) {
                        _imageFormatFailed = 1;
                        fprintf(stderr,
                                "LDVVideo: CGImageCreate 全部 %d 种 BGRX 格式都失败"
                                "（%dx%d stride=%zu）—— 画面会一直黑\n",
                                LDV_BGRA_FORMAT_COUNT, w, h, _rgbStride);
                    }
                    return;
                }

                /* 换掉这一块上次用过的 CGImage。图层若还在引用它，
                 * 由 CA 自己持有 —— 所以我们能安全地放掉自己这份。 */
                if (_img[slot]) { CGImageRelease(_img[slot]); }
                _img[slot] = img;                 /* 持有 */
                self.layer.contents = (__bridge id)img;
                _rgbShown = slot;
                _presented++;

                if (_presented == 1) {
                    fprintf(stderr,
                            "LDVVideo: 首帧已提交给图层 %dx%d %s %s"
                            "（view %.0fx%.0f，格式 #%d，转换 %.2f ms）\n",
                            w, h,
                            view->layout == LDV_PIXEL_LAYOUT_I420 ? "I420" :
                            (view->layout == LDV_PIXEL_LAYOUT_I422 ? "I422" :
                             (view->layout == LDV_PIXEL_LAYOUT_I444 ? "I444" : "I400")),
                            LDVColorConvertMatrixName(view),
                            self.bounds.size.width, self.bounds.size.height,
                            fi - 1, _lastConvertMs);
                }
            }
        }
    }
}

@end
