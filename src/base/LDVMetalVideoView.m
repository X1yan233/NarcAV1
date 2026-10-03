//
//  LDVMetalVideoView.m — Metal 渲染画布（arm64 专用，设计说明见 .h）
//
//  实现要点（iOS 8 SDK / Xcode 6 时代 API，逐条核对过可用性）：
//
//  1) 着色器用 **运行时编译**（newLibraryWithSource），不引入 .metal 文件
//     —— 工程是脚本生成的，少一种文件类型就少一个 generator 的坑；
//     iOS 8 的 MTLLibrary 运行时编译是一次性开销（毫秒级）。
//
//  2) uniform 对齐坑：MSL 的 float3x3 / float3 在 constant 地址空间
//     每列按 16 字节对齐，直接传 9/3 个 float 会错位。所以 CPU 侧传
//     **float 数组**，shader 里手动组装 —— 数值与 GLES2 路径同表。
//
//  3) 零重新分配：三套（Y/U/V）MTLBuffer 轮转，纹理对象从 buffer
//     **创建一次**（bytesPerRow = dav1d stride，纹理宽 = stride，
//     横向可见范围用 uniform 压 —— 与 GLES2 的 stride 技法一致）。
//     每帧只 memcpy 平面数据进 set k，再用 set k 的纹理编码。
//     深度 3 覆盖 GPU 在途帧数，CPU 覆写 set k 时 GPU 最多在读 k-1/k-2。
//
//  4) I400：1x1 的 128 灰度纹理绑 U/V（与 GLES2 同款兜底）。
//

#import "LDVMetalVideoView.h"

#if defined(__arm64__) || defined(__aarch64__)

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
/*
 * ★ iOS 8.0 SDK 的 QuartzCore umbrella（CoreAnimation.h）**不包含**
 *   CAMetalLayer.h，必须显式 import —— 不写这行就是
 *   "unknown type name 'CAMetalLayer'"（v8.16a 实测）。
 */
#import <QuartzCore/CAMetalLayer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* MSL 着色器（运行时编译，语义与 GLES2 管线逐值一致）                        */
/* ------------------------------------------------------------------------ */

/*
 * MSL 源码：C 字符串数组拼接（ObjC 的 @"" 字面量不能跨裸换行），
 * bootstrap 里转 NSString 交给运行时编译器。行尾统一 \n。
 */
static const char kLDVMetalSource[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct LdvVaryings {\n"
"    float4 position [[position]];\n"
"    float2 texY;\n"
"    float2 texC;\n"
"};\n"
"\n"
"vertex LdvVaryings ldvVertex(uint vid [[vertex_id]],\n"
"                             const device float2 *positions [[buffer(0)]],\n"
"                             const device float2 *texcoords [[buffer(1)]],\n"
"                             const device float *ranges [[buffer(2)]]) {\n"
"    LdvVaryings v;\n"
"    v.position = float4(positions[vid], 0.0, 1.0);\n"
"    float2 t = texcoords[vid];\n"
"    v.texY = float2(t.x * ranges[0], t.y);\n"
"    v.texC = float2(t.x * ranges[1], t.y);\n"
"    return v;\n"
"}\n"
"\n"
"/* 双立方单平面采样：uv 步长 st（每目标像素的 uv 增量），wideX = X 轴用 8-tap。\n"
" * 4-tap 权重 keys(0.5)/keys(1.5)，8-tap 权重 keys(0.25..1.75)/2，行和均为 1。 */\n"
"float ldvBicubicPlane(texture2d<float> t, float2 uv, float2 st,\n"
"                      sampler s, bool wideX) {\n"
"    const float w4[4] = { -0.0625, 0.5625, 0.5625, -0.0625 };\n"
"    const float w8[8] = { -0.01171875, -0.03515625, 0.11328125, 0.43359375,\n"
"                          0.43359375, 0.11328125, -0.03515625, -0.01171875 };\n"
"    float acc = 0.0;\n"
"    for (int j = 0; j < 4; ++j) {\n"
"        float oy = (float(j) - 1.5) * st.y;\n"
"        float row = 0.0;\n"
"        for (int i = 0; i < 8; ++i) {\n"
"            if (i >= 4 && !wideX) continue;\n"
"            float ox = (float(i) - (wideX ? 3.5 : 1.5)) * st.x\n"
"                     * (wideX ? 0.5 : 1.0);\n"
"            row += t.sample(s, uv + float2(ox, oy)).r\n"
"                 * (wideX ? w8[i] : w4[i]);\n"
"        }\n"
"        acc += row * w4[j];\n"
"    }\n"
"    return acc;\n"
"}\n"
"\n"
"fragment float4 ldvFragment(LdvVaryings varyings [[stage_in]],\n"
"                            texture2d<float> texY [[texture(0)]],\n"
"                            texture2d<float> texU [[texture(1)]],\n"
"                            texture2d<float> texV [[texture(2)]],\n"
"                            constant float *mat [[buffer(0)]],\n"
"                            constant float *offsets [[buffer(1)]],\n"
"                            constant float *params [[buffer(3)]]) {\n"
"    constexpr sampler s(address::clamp_to_edge, filter::linear);\n"
"    float3x3 m = float3x3(float3(mat[0], mat[1], mat[2]),\n"
"                          float3(mat[3], mat[4], mat[5]),\n"
"                          float3(mat[6], mat[7], mat[8]));\n"
"    int mode = (int)params[4];\n"
"    float yv, uv2, vv;\n"
"    if (mode == 1 || mode == 2) {\n"
"        /* 双立方档：mode 1 = X/Y 各 4-tap；mode 2 = X 8-tap + Y 4-tap。\n"
"         * Keys cubic（a=-0.5）负瓣轻锐化；抽头位置随缩放步长展宽，\n"
"         * 权重是与步长无关的常数（行和恰为 1）。与 GLES 路径同源。 */\n"
"        const bool wideX = (mode == 2);\n"
"        yv  = ldvBicubicPlane(texY, varyings.texY,\n"
"                              float2(params[0], params[1]), s, wideX);\n"
"        uv2 = ldvBicubicPlane(texU, varyings.texC,\n"
"                              float2(params[2], params[3]), s, wideX);\n"
"        vv  = ldvBicubicPlane(texV, varyings.texC,\n"
"                              float2(params[2], params[3]), s, wideX);\n"
"    } else {\n"
"        yv  = texY.sample(s, varyings.texY).r;\n"
"        uv2 = texU.sample(s, varyings.texC).r;\n"
"        vv  = texV.sample(s, varyings.texC).r;\n"
"    }\n"
"    float3 yuv = float3(yv, uv2, vv) * 255.0\n"
"               - float3(offsets[0], offsets[1], offsets[2]);\n"
"    float3 rgb = m * yuv;\n"
"    return float4(clamp(rgb * (1.0 / 255.0), 0.0, 1.0), 1.0);\n"
"}\n";

/* ------------------------------------------------------------------------ */
/* 色彩矩阵（与 LDVColorConvert.c / GLES2 路径逐值对齐，列主序 9 float）      */
/* ------------------------------------------------------------------------ */

static const float kLDVMetalMat601Video[9] = {
    1.164383f, 1.164383f, 1.164383f,
    0.0f, -0.391762f, 2.017232f,
    1.596027f, -0.812968f, 0.0f
};
static const float kLDVMetalMat709Video[9] = {
    1.164383f, 1.164383f, 1.164383f,
    0.0f, -0.213249f, 2.112402f,
    1.792741f, -0.532909f, 0.0f
};
static const float kLDVMetalMat601Full[9] = {
    1.0f, 1.0f, 1.0f,
    0.0f, -0.344136f, 1.772f,
    1.402f, -0.714136f, 0.0f
};
static const float kLDVMetalMat709Full[9] = {
    1.0f, 1.0f, 1.0f,
    0.0f, -0.1873f, 1.8556f,
    1.5748f, -0.4681f, 0.0f
};

/* 与 LDVColorConvert.c 的 ldv_use_bt709 一致（含分辨率兜底） */
static int ldv_metal_use_bt709(const LDVFrameView *v) {
    switch (v->matrix) {
    case LDV_MATRIX_BT709:      return 1;
    case LDV_MATRIX_BT601:      return 0;
    case LDV_MATRIX_BT470BG:    return 0;
    case LDV_MATRIX_BT2020_NCL: return 1;
    case LDV_MATRIX_BT2020_CL:  return 1;
    default:                    break;
    }
    return v->height > 576;
}

/* 按布局返回色度平面尺寸；I400 返回 0 */
static int ldv_metal_chroma_dims(const LDVFrameView *v, int *cw, int *ch) {
    switch (v->layout) {
    case LDV_PIXEL_LAYOUT_I420: *cw = (v->width + 1) / 2; *ch = (v->height + 1) / 2; return 1;
    case LDV_PIXEL_LAYOUT_I422: *cw = (v->width + 1) / 2; *ch = v->height; return 1;
    case LDV_PIXEL_LAYOUT_I444: *cw = v->width; *ch = v->height; return 1;
    default: return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* 平面资源组（三套轮转）                                                    */
/* ------------------------------------------------------------------------ */

@interface LDVMetalPlaneSet : NSObject
@property (nonatomic, strong) id<MTLBuffer> yBuf, uBuf, vBuf;
@property (nonatomic, strong) id<MTLTexture> yTex, uTex, vTex;
@end
@implementation LDVMetalPlaneSet
@end

/* ------------------------------------------------------------------------ */

@implementation LDVMetalCanvas {
    id<MTLDevice>            _device;
    id<MTLCommandQueue>      _queue;
    id<MTLLibrary>           _library;   /* 运行时编译的 MSL（v8.16a 漏声明过） */
    id<MTLRenderPipelineState>_pipeline;

    id<MTLBuffer> _posBuf;      /* 8 float，letterbox 位置（clip 坐标） */
    id<MTLBuffer> _texBuf;      /* 8 float，基准纹理坐标（静态） */
    id<MTLBuffer> _rangeBuf;    /* 2 float，uYRange/uCRange */
    id<MTLBuffer> _matBuf;      /* 9 float，色彩矩阵 */
    id<MTLBuffer> _offBuf;      /* 3 float，(yOff,128,128) */
    id<MTLBuffer> _paramBuf;    /* 8 float：[0..3]=uv 步长 L/C，[4]=采样档位 */

    LDVMetalPlaneSet *_sets[3]; /* 三套平面轮转 */
    int               _ring;    /* 本帧写入/使用的 set */
    id<MTLTexture>    _grayTex; /* I400 兜底 */

    /* 资源 key：变化才重建 */
    int _resKeyW, _resKeyH, _resKeySW, _resKeySH, _resKeyLayout;

    /* uniform key */
    int _matrixKey;
    int _rangeKeyW, _rangeKeyCW;

    /* 几何 key */
    int _posKeyVW, _posKeyVH;

    int _backingW, _backingH;

    double _lastUploadMs, _lastDrawMs;
    int    _broken;
}

+ (Class)layerClass {
    /*
     * CAMetalLayer 是 iOS 8 的类；本类只会在 isSupported=YES 之后被
     * 实例化，但兜底写法不赌 —— 类不在就退普通 CALayer（那样 drawFrame
     * 会因 drawable 建不起来而置 broken，走降级路径）。
     */
    Class c = NSClassFromString(@"CAMetalLayer");
    return c ? c : [CALayer class];
}

+ (BOOL)isSupported {
    /* CAMetalLayer 只在 iOS 8+ 存在 —— 这是"系统能力"的探针 */
    if (!NSClassFromString(@"CAMetalLayer")) return NO;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    return dev != nil;
}

- (id)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.opaque = YES;
        UIScreen *ms = [UIScreen mainScreen];
        if (ms) self.contentScaleFactor = ms.scale;
    }
    return self;
}

- (void)dealloc {
    /* ARC：Metal 对象随 ivar 释放；显式清引用便于阅读生命周期 */
    _pipeline = nil;
    _library = nil;
    _queue = nil;
    _device = nil;
}

#pragma mark 初始化

- (BOOL)bootstrap {
    _device = MTLCreateSystemDefaultDevice();
    if (!_device) {
        fprintf(stderr, "LDVMetal: 无默认 device\n");
        return NO;
    }
    _queue = [_device newCommandQueue];
    if (!_queue) return NO;

    {
        NSError *err = nil;
        NSString *src = [NSString stringWithUTF8String:kLDVMetalSource];
        _library = [_device newLibraryWithSource:src
                                         options:nil
                                           error:&err];
        if (!_library) {
            fprintf(stderr, "LDVMetal: 着色器编译失败 %s\n",
                    err.localizedDescription.UTF8String ?: "?");
            return NO;
        }
    }

    {
        /*
         * ★ iOS 8.0 SDK 没有 [MTLRenderPipelineDescriptor
         *   renderPipelineDescriptor] 这个便利构造器（后来 SDK 才加），
         *   用 alloc/init —— 语义完全等价。
         */
        MTLRenderPipelineDescriptor *p =
            [[MTLRenderPipelineDescriptor alloc] init];
        p.vertexFunction   = [_library newFunctionWithName:@"ldvVertex"];
        p.fragmentFunction = [_library newFunctionWithName:@"ldvFragment"];
        p.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (!p.vertexFunction || !p.fragmentFunction) {
            fprintf(stderr, "LDVMetal: 找不到着色器入口函数\n");
            return NO;
        }
        NSError *err = nil;
        _pipeline = [_device newRenderPipelineStateWithDescriptor:p
                                                            error:&err];
        if (!_pipeline) {
            fprintf(stderr, "LDVMetal: pipeline 创建失败 %s\n",
                    err.localizedDescription.UTF8String ?: "?");
            return NO;
        }
    }

    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    if (![l isKindOfClass:[CAMetalLayer class]]) {
        fprintf(stderr, "LDVMetal: 宿主层不是 CAMetalLayer\n");
        return NO;
    }
    l.device       = _device;
    l.pixelFormat  = MTLPixelFormatBGRA8Unorm;   /* 与 pipeline 声明一致 */
    l.framebufferOnly = YES;   /* TBDR 优化：不做 CPU 回读 */

    /* 几何：基准纹理坐标（静态），位置缓冲占位 */
    {
        /*
         * ★ options 只用 CPUCacheMode，不写 MTLResourceStorageModeShared：
         *   StorageMode 选项位是 **iOS 9** 才加入 MTLResourceOptions 的，
         *   Xcode 6.0.1 的 iOS 8.0 SDK 里没有这个标识符（编译直接炸，
         *   v8.16 首版实测）。iOS 8 的统一内存架构下 buffer 天然就是
         *   shared 语义，CPU 写 contents / GPU 读纹理，无需声明。
         *   平面缓冲用 WriteCombined（CPU 每帧整块写、从不读），
         *   uniform/几何用 DefaultCache。
         */
        const float tex[8] = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f };
        _texBuf = [_device newBufferWithBytes:tex
                                       length:sizeof(tex)
                                      options:0 /* 默认 CPU cache（iOS 8 无 OptionCPUCacheModeDefault 之外的必要性） */];
        _posBuf = [_device newBufferWithLength:8 * sizeof(float)
                                       options:0 /* 默认 CPU cache（iOS 8 无 OptionCPUCacheModeDefault 之外的必要性） */];
        _rangeBuf = [_device newBufferWithLength:2 * sizeof(float)
                                         options:0 /* 默认 CPU cache（iOS 8 无 OptionCPUCacheModeDefault 之外的必要性） */];
        _matBuf = [_device newBufferWithLength:9 * sizeof(float)
                                       options:0 /* 默认 CPU cache（iOS 8 无 OptionCPUCacheModeDefault 之外的必要性） */];
        _offBuf = [_device newBufferWithLength:3 * sizeof(float)
                                       options:0 /* 默认 CPU cache（iOS 8 无 OptionCPUCacheModeDefault 之外的必要性） */];
        /*
         * ★ _paramBuf 的创建别再丢 —— v8.19d 真机实锤：只加了 drawFrame
         *   里的 contents 写入和 encoder 绑定，创建编辑没落盘，ivar 为 nil
         *   → .contents 返回 NULL → p[0]= 写地址 0x0 → SE1 秒崩
         *   （EXC_BAD_ACCESS KERN_INVALID_ADDRESS at 0x0，.ips 栈就在
         *   drawFrame）。三件套：创建 / 填充 / 绑定，缺一即崩或黑屏。
         */
        _paramBuf = [_device newBufferWithLength:8 * sizeof(float)
                                         options:0 /* 默认 CPU cache（iOS 8 无 OptionCPUCacheModeDefault 之外的必要性） */];
        if (!_texBuf || !_posBuf || !_rangeBuf || !_matBuf || !_offBuf ||
            !_paramBuf) {
            return NO;
        }
    }

    return YES;
}

#pragma mark 资源

/* (re)建三套平面 buffer + 纹理。dims/stride/布局变了才重建。 */
- (BOOL)ensureResources:(const LDVFrameView *)view {
    const int sw = (int)view->stride[0];
    const int sh = view->height;
    int cw = 0, ch = 0;
    const int hasChroma = ldv_metal_chroma_dims(view, &cw, &ch);
    const int scw = hasChroma ? (int)view->stride[1] : 0;
    const int sch = hasChroma ? ch : 0;

    if (_sets[0] && _resKeyW == sw && _resKeyH == sh &&
        _resKeySW == scw && _resKeySH == sch &&
        _resKeyLayout == (int)view->layout) {
        return YES;
    }

    for (int i = 0; i < 3; i++) {
        LDVMetalPlaneSet *set = [[LDVMetalPlaneSet alloc] init];
        set.yBuf = [_device newBufferWithLength:(NSUInteger)sw * sh
                                        options:MTLResourceOptionCPUCacheModeWriteCombined];
        if (hasChroma) {
            set.uBuf = [_device newBufferWithLength:(NSUInteger)scw * sch
                                            options:MTLResourceOptionCPUCacheModeWriteCombined];
            set.vBuf = [_device newBufferWithLength:(NSUInteger)scw * sch
                                            options:MTLResourceOptionCPUCacheModeWriteCombined];
        }
        if (!set.yBuf || (hasChroma && (!set.uBuf || !set.vBuf))) {
            fprintf(stderr, "LDVMetal: 平面 buffer 分配失败\n");
            return NO;
        }

        MTLTextureDescriptor *d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                         width:sw height:sh mipmapped:NO];
        set.yTex = [set.yBuf newTextureWithDescriptor:d offset:0
                                          bytesPerRow:(NSUInteger)sw];
        if (hasChroma) {
            MTLTextureDescriptor *cd = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                             width:scw height:sch mipmapped:NO];
            set.uTex = [set.uBuf newTextureWithDescriptor:cd offset:0
                                              bytesPerRow:(NSUInteger)scw];
            set.vTex = [set.vBuf newTextureWithDescriptor:cd offset:0
                                              bytesPerRow:(NSUInteger)scw];
            if (!set.uTex || !set.vTex) return NO;
        }
        if (!set.yTex) return NO;
        _sets[i] = set;
    }

    /* I400 兜底：1x1 的 128 灰度（与 GLES2 路径同款） */
    if (!hasChroma && !_grayTex) {
        id<MTLBuffer> g = [_device newBufferWithLength:1
                                               options:0 /* 默认 CPU cache（iOS 8 无 OptionCPUCacheModeDefault 之外的必要性） */];
        if (!g) return NO;
        ((unsigned char *)g.contents)[0] = 128;
        MTLTextureDescriptor *gd = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                         width:1 height:1 mipmapped:NO];
        _grayTex = [g newTextureWithDescriptor:gd offset:0 bytesPerRow:1];
        if (!_grayTex) return NO;
    }

    _resKeyW = sw;  _resKeyH = sh;
    _resKeySW = scw; _resKeySH = sch;
    _resKeyLayout = (int)view->layout;
    _rangeKeyW = 0; _rangeKeyCW = 0;   /* range 依赖 stride/布局 */
    return YES;
}

#pragma mark 渲染

- (BOOL)canRenderFrame:(const LDVFrameView *)view {
    int cw = 0, ch = 0;

    if (_broken || !_pipeline) return NO;
    if (view->bpc != 8) return NO;
    if (!view->plane[0] || view->width <= 0 || view->height <= 0) return NO;
    if (view->stride[0] < view->width) return NO;
    if (view->stride[0] > 16384 || view->height > 16384) return NO;
    /* Metal stride 纹理的 bytesPerRow 必须 64 字节对齐 —— AGX 驱动的
     * validateStrideTextureParameters 对不对齐直接 abort（断言，无法
     * 捕获；v8.21 SE1 实锤）。不对齐就不选 Metal，让这帧走 GLES/CPU。
     * 本库自产的帧（dav1d 原生 128 对齐 / ES2 转换器 64 对齐）都满足。 */
    if (view->stride[0] & 63) return NO;
    if (view->layout == LDV_PIXEL_LAYOUT_I400) return YES;
    if (!ldv_metal_chroma_dims(view, &cw, &ch)) return NO;
    if (!view->plane[1] || !view->plane[2]) return NO;
    if (view->stride[1] < cw) return NO;
    if (view->stride[1] > 16384 || ch > 16384) return NO;
    if (view->stride[1] & 63) return NO;
    return YES;
}

/* letterbox：与 GLES2 路径同一套 fit 公式 */
- (void)updateGeometryVideoW:(int)vw h:(int)vh {
    float pos[8];
    double bw = (double)_backingW, bh = (double)_backingH;
    double scale = (bw / (double)vw < bh / (double)vh)
                        ? bw / (double)vw : bh / (double)vh;
    double dw = (double)vw * scale, dh = (double)vh * scale;
    double px0 = (bw - dw) / 2.0, py0 = (bh - dh) / 2.0;
    double fx0, fx1, fyTop, fyBot;

    if (dw < 1.0) dw = 1.0;
    if (dh < 1.0) dh = 1.0;
    fx0   = -1.0 + 2.0 * px0 / bw;
    fx1   = -1.0 + 2.0 * (px0 + dw) / bw;
    fyTop = 1.0 - 2.0 * py0 / bh;
    fyBot = 1.0 - 2.0 * (py0 + dh) / bh;

    pos[0] = (float)fx0; pos[1] = (float)fyTop;
    pos[2] = (float)fx1; pos[3] = (float)fyTop;
    pos[4] = (float)fx0; pos[5] = (float)fyBot;
    pos[6] = (float)fx1; pos[7] = (float)fyBot;

    memcpy(_posBuf.contents, pos, sizeof(pos));
    _posKeyVW = vw;
    _posKeyVH = vh;
}

- (BOOL)drawFrame:(const LDVFrameView *)view {
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    int cw = 0, ch = 0, hasChroma;
    int wantW, wantH, mkey;
    double t0, t1, t2;

    if (_broken) return NO;
    if (![l isKindOfClass:[CAMetalLayer class]]) { _broken = 1; return NO; }

    /* drawable 尺寸：bounds x contentsScale，变了就更新（几何 key 失效） */
    wantW = (int)(l.bounds.size.width  * l.contentsScale + 0.5);
    wantH = (int)(l.bounds.size.height * l.contentsScale + 0.5);
    if (wantW <= 0 || wantH <= 0) return NO;   /* 还没布局：瞬时 */
    if (wantW != _backingW || wantH != _backingH) {
        l.drawableSize = CGSizeMake((CGFloat)wantW, (CGFloat)wantH);
        _backingW = wantW;
        _backingH = wantH;
        _posKeyVW = 0;
        _posKeyVH = 0;
    }

    t0 = CACurrentMediaTime();

    hasChroma = ldv_metal_chroma_dims(view, &cw, &ch);
    if (![self ensureResources:view]) { _broken = 1; return NO; }

    /* ---- 平面上传：memcpy 进轮转 set（纹理对象不重建） ---- */
    {
        LDVMetalPlaneSet *set = _sets[_ring];
        const size_t yBytes = (size_t)view->stride[0] * (size_t)view->height;
        memcpy(set.yBuf.contents, view->plane[0], yBytes);
        if (hasChroma) {
            const size_t cBytes = (size_t)view->stride[1] * (size_t)ch;
            memcpy(set.uBuf.contents, view->plane[1], cBytes);
            memcpy(set.vBuf.contents, view->plane[2], cBytes);
        }
    }

    /* ---- uniforms（key 变了才更新） ---- */
    mkey = ((int)view->matrix << 1) | (view->colorRange ? 1 : 0);
    if (mkey != _matrixKey) {
        float *mat = (float *)_matBuf.contents;
        const float *m = ldv_metal_use_bt709(view)
            ? (view->colorRange ? kLDVMetalMat709Full : kLDVMetalMat709Video)
            : (view->colorRange ? kLDVMetalMat601Full : kLDVMetalMat601Video);
        float *off = (float *)_offBuf.contents;
        memcpy(mat, m, 9 * sizeof(float));
        off[0] = view->colorRange ? 0.0f : 16.0f;
        off[1] = 128.0f;
        off[2] = 128.0f;
        _matrixKey = mkey;
        _rangeKeyW = 0;
        _rangeKeyCW = 0;
    }
    /* range 独立刷新：首次、stride/布局变化、以及矩阵 key 刚失效时都走到这 */
    if (_rangeKeyW != view->width || _rangeKeyCW != cw) {
        float *r = (float *)_rangeBuf.contents;
        r[0] = (float)view->width  / (float)view->stride[0];
        r[1] = hasChroma ? (float)cw / (float)view->stride[1] : 1.0f;
        _rangeKeyW = view->width;
        _rangeKeyCW = cw;
    }

    /*
     * ---- 采样档位与 uv 步长 ----
     *
     * ★ 这块**必须每帧写**：_paramBuf 是渲染参数的唯一来源，v8.19 首版
     *   只建了 buffer 没写 contents —— 全 0 意味着 mode 恒 0（双立方
     *   永不生效）。直写共享内存 32 字节，成本可忽略。
     * letterbox 等比缩放：r = 1/scale。档位与 GLES 路径同源：
     *   r < 1.5 双线性；1.5 ≤ r < 3 → 8-tap；r ≥ 3 → 12-tap（X 宽核）。
     */
    {
        const double bw2 = (double)_backingW, bh2 = (double)_backingH;
        const double sc  = (bw2 / (double)view->width < bh2 / (double)view->height)
                               ? bw2 / (double)view->width
                               : bh2 / (double)view->height;
        const double dw = (double)view->width * sc;
        const double dh = (double)view->height * sc;
        const double r  = (sc > 0.0) ? 1.0 / sc : 1.0;
        const int    smode = (r >= 3.0) ? 2 : ((r >= 1.5) ? 1 : 0);
        float       *p      = (float *)_paramBuf.contents;

        p[0] = (float)((double)view->width  / dw / (double)view->stride[0]);
        p[1] = (float)(1.0 / dh);
        p[2] = hasChroma
                   ? (float)((double)cw / dw / (double)view->stride[1])
                   : 0.0f;
        p[3] = hasChroma ? (float)(1.0 / dh) : 0.0f;
        p[4] = (float)smode;
    }

    /* ---- 几何 ---- */
    if (_posKeyVW != view->width || _posKeyVH != view->height) {
        [self updateGeometryVideoW:view->width h:view->height];
    }

    t1 = CACurrentMediaTime();

    /* ---- 编码与提交 ---- */
    {
        id<CAMetalDrawable> d = [l nextDrawable];
        id<MTLCommandBuffer> cmd;
        MTLRenderPassDescriptor *rp;
        id<MTLRenderCommandEncoder> enc;
        LDVMetalPlaneSet *set = _sets[_ring];

        if (!d) return NO;   /* 瞬时：drawable 池暂时空 */
        cmd = [_queue commandBuffer];
        if (!cmd) { _broken = 1; return NO; }

        rp = [[MTLRenderPassDescriptor alloc] init];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        enc = [cmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:_pipeline];
        [enc setVertexBuffer:_posBuf   offset:0 atIndex:0];
        [enc setVertexBuffer:_texBuf   offset:0 atIndex:1];
        [enc setVertexBuffer:_rangeBuf offset:0 atIndex:2];
        [enc setFragmentTexture:set.yTex atIndex:0];
        [enc setFragmentTexture:(hasChroma ? set.uTex : _grayTex) atIndex:1];
        [enc setFragmentTexture:(hasChroma ? set.vTex : _grayTex) atIndex:2];
        [enc setFragmentBuffer:_matBuf offset:0 atIndex:0];
        [enc setFragmentBuffer:_offBuf offset:0 atIndex:1];
        [enc setFragmentBuffer:_paramBuf offset:0 atIndex:3];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];

        [cmd presentDrawable:d];
        [cmd commit];
        _ring = (_ring + 1) % 3;
    }

    t2 = CACurrentMediaTime();
    _lastUploadMs = (t1 - t0) * 1000.0;
    _lastDrawMs   = (t2 - t1) * 1000.0;
    return YES;
}

- (void)clearToBlack {
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    id<CAMetalDrawable> d;
    id<MTLCommandBuffer> cmd;
    MTLRenderPassDescriptor *rp;
    id<MTLRenderCommandEncoder> enc;

    if (_broken || ![l isKindOfClass:[CAMetalLayer class]]) return;
    d = [l nextDrawable];
    if (!d) return;
    cmd = [_queue commandBuffer];
    if (!cmd) return;
    rp = [[MTLRenderPassDescriptor alloc] init];
    rp.colorAttachments[0].texture = d.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc endEncoding];
    [cmd presentDrawable:d];
    [cmd commit];
}

- (BOOL)isBroken { return _broken != 0; }
- (double)lastUploadMilliseconds { return _lastUploadMs; }
- (double)lastDrawMilliseconds   { return _lastDrawMs; }

@end

#endif /* __arm64__ */
