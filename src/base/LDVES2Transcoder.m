/*
 * LDVES2Transcoder.m — ES2 异步缩小转换器的实现（v8.20）
 *
 * 设计要点（详见头文件）：
 *   - 独立 EAGLContext（ES2，无 drawable、无 sharegroup），offscreen FBO
 *   - 源：三张 GL_LUMINANCE 纹理，纹理宽 = stride（零拷贝上传的老办法，
 *     ES2 没有 GL_UNPACK_ROW_LENGTH），可见宽用 uniform 压纹理坐标
 *   - 目标：一张 RGBA FBO 纹理，片元着色器对 Y/U/V 各一次 GL_LINEAR
 *     采样（armv7 定案：二次缩小用线性），打包 gl_FragColor=(y,u,v,1)
 *   - 回读：glReadPixels(GL_RGBA/GL_UNSIGNED_BYTE) —— ES2 唯一保证
 *     支持的组合；CPU 解包回 I420 三平面（一次 malloc 整块，缓存
 *     PushOwned 直接接管）
 *   - GL 失败/纹理超限 → _glBroken：直通模式（原样进缓存），不黑屏
 *
 * y 方向说明：顶点数据把 t=0 放在 clip y=-1（GL 窗口底），因此
 * glReadPixels 读出的第 0 行就是源图像顶行 —— 解包按内存顺序直写，
 * 不再翻转。
 */

#import "LDVES2Transcoder.h"
#import "LDVFrameCache.h"

#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>

#import <pthread.h>
#import <stdlib.h>
#import <string.h>
#import <sys/time.h>

/* 队列深度：4 帧原生帧（720p ≈ 5.5MB）。转换吞吐远高于解码吞吐，
 * 实际几乎不会超过 1-2 帧；4 是"解码线程完全不等"的安全余量。 */
#define LDV_ES2_TC_QUEUE_SLOTS 4

typedef struct LDVEs2TcSlot {
    uint8_t      *owner;       /* 一次 malloc 的整块：Y|U|V（源布局） */
    size_t        ownerBytes;  /* 计入缓存字节预算的量 */
    LDVFrameView  view;        /* plane 指向 owner 内部 */
} LDVEs2TcSlot;

/* ------------------------------------------------------------------------ */
/* 着色器                                                                    */
/* ------------------------------------------------------------------------ */

static const char *ldv_es2_tc_vs = {
    "attribute vec4 a_pos;"          /* x, y, s, t                          */
    "varying mediump vec2 v_tex;"
    "void main() {"
    "    gl_Position = vec4(a_pos.xy, 0.0, 1.0);"
    "    v_tex = a_pos.zw;"
    "}"
};

static const char *ldv_es2_tc_fs = {
    /* 采样缩小用 GL_LINEAR（armv7 定案：二次线性）；x 方向乘可见比例，
     * 把"纹理宽 = stride"的 padding 裁掉。 */
    "precision highp float;"
    "varying mediump vec2 v_tex;"
    "uniform sampler2D s_y;"
    "uniform sampler2D s_u;"
    "uniform sampler2D s_v;"
    "uniform highp float u_visY;"    /* 可见宽 / strideY，<= 1 */
    "uniform highp float u_visUV;"   /* 色度可见宽 / strideUV，<= 1 */
    "void main() {"
    "    mediump float y = texture2D(s_y, vec2(v_tex.x * u_visY,  v_tex.y)).r;"
    "    mediump float u = texture2D(s_u, vec2(v_tex.x * u_visUV, v_tex.y)).r;"
    "    mediump float v = texture2D(s_v, vec2(v_tex.x * u_visUV, v_tex.y)).r;"
    "    gl_FragColor = vec4(y, u, v, 1.0);"
    "}"
};

/* workerLoop 被下面的 C 线程入口调用 —— ARC 下 C 函数可见的 ObjC 方法
 * 声明必须出现在调用点之前（class extension 前置），否则编译报
 * "No visible @interface for 'LDVES2Transcoder' declares the selector
 * 'workerLoop'"（v8.21 教训：声明放在入口函数之后无效）。 */
@interface LDVES2Transcoder ()
- (void)workerLoop;
@end

/* 线程入口：把 self 的引用转交给线程（init 里 bridge_retained +1，
 * 这里 bridge_transfer 消费掉），对象在线程退出前始终有效。 */
static void *ldv_es2_tc_thread_entry(void *arg) {
    LDVES2Transcoder *tc = (__bridge_transfer LDVES2Transcoder *)arg;
    [tc workerLoop];
    return NULL;
}

/* 带超时的条件等待（100ms），醒来重查停止标志 —— 退出永不卡死。 */
static void ldv_es2_tc_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    struct timeval  tv;
    struct timespec ts;

    gettimeofday(&tv, NULL);
    ts.tv_sec  = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000 + 100 * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(cond, mutex, &ts);
}

/* 纹理采样参数：线性过滤 + CLAMP_TO_EDGE（四个纹理共用）。
 * 以普通 C 函数实现 —— ObjC 方法无法被 C 上下文直接调用
 * （v8.21 教训："Implicit declaration of function is invalid in C99"）。 */
static void ldv_es2_tc_tex_params(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

@implementation LDVES2Transcoder {
    /* —— 外部接线 —— */
    LDVFrameCache *_cache;          /* 显示缓存（不持有；先 stop 再销毁缓存） */

    /* —— 同步原语与队列（ingest / worker / setOutput / stop 四方共用） —— */
    pthread_mutex_t _mutex;
    pthread_cond_t  _cond;
    pthread_cond_t  _spaceCond;     /* ingest 等队列空位（缓存满背压链） */
    LDVEs2TcSlot    _slots[LDV_ES2_TC_QUEUE_SLOTS];
    int             _head;
    int             _count;

    /* —— 状态（锁内读写） —— */
    int             _dstW;          /* 当前缩小目标（0 = 直通） */
    int             _dstH;
    int             _stopFlag;
    int64_t         _dropped;       /* 队列满被丢的帧数（诊断） */

    /* —— 转换线程 —— */
    pthread_t       _thread;
    int             _threadValid;

    /* —— GL 资源（仅转换线程触碰；_glBroken 后不再动） —— */
    EAGLContext    *_ctx;
    GLuint          _texY, _texU, _texV;    /* 源平面（宽 = stride） */
    GLuint          _fboTex;                /* RGBA 缩小产物 */
    GLuint          _fbo;
    GLuint          _prog;
    GLint           _attrPos;
    GLint           _uVisY, _uVisUV;
    GLint           _maxTex;                /* GL_MAX_TEXTURE_SIZE */
    int             _srcW, _srcH;           /* 已分配源纹理的尺寸 */
    ptrdiff_t       _srcStrideY, _srcStrideUV;
    int             _fboW, _fboH;           /* 已分配 FBO/readback 的尺寸 */
    uint8_t        *_readback;              /* RGBA 回读缓冲（dstW*dstH*4） */
    int             _glBroken;              /* 1 = 直通兜底，GL 不再触碰 */
}

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

- (id)initWithCache:(LDVFrameCache *)cache {
    self = [super init];
    if (!self) return nil;
    if (!cache) return nil;

    _cache = cache;

    if (pthread_mutex_init(&_mutex, NULL) != 0) return nil;
    if (pthread_cond_init(&_cond, NULL) != 0) {
        pthread_mutex_destroy(&_mutex);
        return nil;
    }
    if (pthread_cond_init(&_spaceCond, NULL) != 0) {
        pthread_cond_destroy(&_cond);
        pthread_mutex_destroy(&_mutex);
        return nil;
    }

    _head = 0;
    _count = 0;
    _dstW = 0;
    _dstH = 0;
    _maxTex = 0;
    _texY = _texU = _texV = _fboTex = _fbo = _prog = 0;

    /* 转换线程。__bridge_retained 把一个引用转交给线程，
     * 入口函数里 __bridge_transfer 接走 —— 线程活着对象就活着。 */
    if (pthread_create(&_thread, NULL, ldv_es2_tc_thread_entry,
                       (__bridge_retained void *)self) != 0) {
        pthread_cond_destroy(&_cond);
        pthread_mutex_destroy(&_mutex);
        return nil;
    }
    _threadValid = 1;
    return self;
}

- (void)dealloc {
    [self stop];
}

- (void)stop {
    pthread_t thread;
    int       joinable;

    pthread_mutex_lock(&_mutex);
    if (_stopFlag) {
        /* v8.21b 幂等：stop 可能被调用多次（exitToHome 一次、dealloc
         * 一次）—— 原语 destroy 只能执行一次，二次调用直接返回。 */
        pthread_mutex_unlock(&_mutex);
        return;
    }
    _stopFlag = 1;
    pthread_cond_broadcast(&_cond);
    pthread_cond_broadcast(&_spaceCond);   /* 唤醒可能背压中的 ingest */
    thread = _thread;
    joinable = _threadValid;
    _threadValid = 0;
    pthread_mutex_unlock(&_mutex);

    /* 等转换线程退出：它退出前会自己释放全部 GL 资源并 setCurrentContext:nil。
     * 之后缓存销毁才是安全的（不会再有 Push 并发进来）。 */
    if (joinable) pthread_join(thread, NULL);

    pthread_cond_destroy(&_spaceCond);
    pthread_cond_destroy(&_cond);
    pthread_mutex_destroy(&_mutex);
}

/* ------------------------------------------------------------------------ */
/* 生产者（解码线程）                                                        */
/* ------------------------------------------------------------------------ */

- (void)ingestFrameView:(const LDVFrameView *)view {
    if (!view || view->width <= 0 || view->height <= 0 || !view->plane[0]) {
        return;
    }

    /* 高位深 / 非 I420：不进 GL，按原尺寸直推缓存（v8.19f 的原生行为）。 */
    if (view->bpc != 8 || view->layout != LDV_PIXEL_LAYOUT_I420) {
        [self pushOriginalView:view];
        return;
    }

    /* 复制进自有缓冲（ingest 返回后借用内存作废）。 */
    size_t   yB   = (size_t)view->stride[0] * (size_t)view->height;
    size_t   uvH  = (size_t)((view->height + 1) / 2);
    size_t   uvB  = (size_t)view->stride[1] * uvH;
    uint8_t *owner = (uint8_t *)malloc(yB + 2 * uvB);
    if (!owner) return;

    memcpy(owner, view->plane[0], yB);
    if (uvB) {
        memcpy(owner + yB, view->plane[1], uvB);
        memcpy(owner + yB + uvB, view->plane[2], uvB);
    }

    pthread_mutex_lock(&_mutex);
    if (_stopFlag) {
        pthread_mutex_unlock(&_mutex);
        free(owner);
        return;
    }
    while (_count >= LDV_ES2_TC_QUEUE_SLOTS && !_stopFlag) {
        /* 队列满：等转换线程消费腾位（v8.21b 背压，不再丢帧）。
         * 丢帧会压缩缓存序号 → 显示节拍失真 → 内容倍速快进
         * （v8.21 SE1 实测）。缓存满时转换线程阻塞在 push 上，
         * 显示 Pop 腾位后整条链路逐级苏醒。100ms timedwait：
         * 停止/异常下退出永不卡死。_dropped 改记背压次数。 */
        ldv_es2_tc_timedwait(&_spaceCond, &_mutex);
        _dropped++;
    }
    if (_stopFlag) {
        pthread_mutex_unlock(&_mutex);
        free(owner);
        return;
    }
    {
        LDVEs2TcSlot *slot = &_slots[(_head + _count) % LDV_ES2_TC_QUEUE_SLOTS];
        slot->owner      = owner;
        slot->ownerBytes = yB + 2 * uvB;
        slot->view       = *view;
        slot->view.plane[0] = owner;
        slot->view.plane[1] = owner + yB;
        slot->view.plane[2] = owner + yB + uvB;
        slot->view.index = 0;              /* 由缓存自己填 */
        _count++;
    }
    pthread_cond_signal(&_cond);
    pthread_mutex_unlock(&_mutex);
}

- (void)clearQueuedFrames {
    pthread_mutex_lock(&_mutex);
    while (_count > 0) {
        LDVEs2TcSlot *slot = &_slots[_head];
        free(slot->owner);
        slot->owner = NULL;
        _head = (_head + 1) % LDV_ES2_TC_QUEUE_SLOTS;
        _count--;
    }
    pthread_mutex_unlock(&_mutex);
}

- (void)setOutputWidth:(int)w height:(int)h {
    int tw = (w > 2) ? (w & ~1) : ((w > 0) ? 2 : 0);
    int th = (h > 2) ? (h & ~1) : ((h > 0) ? 2 : 0);

    pthread_mutex_lock(&_mutex);
    _dstW = tw;
    _dstH = th;
    pthread_cond_signal(&_cond);
    pthread_mutex_unlock(&_mutex);
}

/* ------------------------------------------------------------------------ */
/* 转换线程                                                                  */
/* ------------------------------------------------------------------------ */

- (void)workerLoop {
    /* ARC 禁 NSAutoreleasePool —— 用 @autoreleasepool 块包住线程主体。 */
    @autoreleasepool {
    [self setupGL];

    for (;;) {
        int           stopNow = 0;
        int           dstW, dstH;
        int           hasSlot = 0;
        LDVEs2TcSlot  slot;

        pthread_mutex_lock(&_mutex);
        while (_count == 0 && !_stopFlag) {
            ldv_es2_tc_timedwait(&_cond, &_mutex);
        }
        stopNow = _stopFlag;
        dstW = _dstW;
        dstH = _dstH;
        if (!stopNow && _count > 0) {
            slot = _slots[_head];
            _slots[_head].owner = NULL;
            _head = (_head + 1) % LDV_ES2_TC_QUEUE_SLOTS;
            _count--;
            hasSlot = 1;
            /* 腾出一个槽位：唤醒在 ingest 里背压等待的解码线程 */
            pthread_cond_broadcast(&_spaceCond);
        }
        pthread_mutex_unlock(&_mutex);

        if (stopNow) {
            if (hasSlot) {
                /* 停止时的当前帧直接还给缓存（若缓存也快没了，Push 会拒绝，
                 * 那就 free —— 两条路都安全）。 */
                [self pushSlotDirectly:&slot];
            }
            /* 队列残余帧全部归还缓存或释放，绝不泄漏。
             * 锁外 push（PushOwned 内部有缓存自己的锁，不能嵌套）。 */
            for (;;) {
                LDVEs2TcSlot rest;
                pthread_mutex_lock(&_mutex);
                if (_count == 0) {
                    pthread_mutex_unlock(&_mutex);
                    break;
                }
                rest = _slots[_head];
                _slots[_head].owner = NULL;
                _head = (_head + 1) % LDV_ES2_TC_QUEUE_SLOTS;
                _count--;
                pthread_mutex_unlock(&_mutex);
                [self pushSlotDirectly:&rest];
            }
            break;
        }

        if (!hasSlot) continue;

        if (_glBroken || dstW <= 0 || dstH <= 0) {
            /* 兜底 / 直通：原样进缓存 */
            [self pushSlotDirectly:&slot];
        } else {
            [self transcodeSlot:&slot dstW:dstW dstH:dstH];
        }
    }

    [self teardownGL];
    }
}

/*
 * 直推：slot 的 owner（已是自有整块缓冲）所有权移交给缓存。
 * 缓存满 → 背压等待重试，不丢帧（理由同 transcodeSlot：丢帧压缩缓存
 * 序号 → 显示节拍失真 → 内容倍速快进）。停止时直接释放，绝不背压。
 */
- (void)pushSlotDirectly:(LDVEs2TcSlot *)slot {
    if (!slot->owner) return;
    for (;;) {
        LDVResult r = LDVFrameCachePushOwned(_cache, &slot->view,
                                             slot->owner, slot->ownerBytes);
        if (r == LDV_OK) break;
        if (_stopFlag || r != LDV_ERR_NEED_INPUT) {
            free(slot->owner);       /* 停止/其它错误：释放 */
            break;
        }
        LDVFrameCacheWaitForSpace(_cache, 100);
    }
    slot->owner = NULL;
}

/* ingest 阶段的直推（借用内存 → 复制 → 推缓存）。 */
- (void)pushOriginalView:(const LDVFrameView *)view {
    size_t   yB  = (size_t)view->stride[0] * (size_t)view->height;
    size_t   uvH = (size_t)((view->height + 1) / 2);
    size_t   uvB = (view->layout == LDV_PIXEL_LAYOUT_I400)
                 ? 0 : (size_t)view->stride[1] * uvH;
    uint8_t *owner;

    if (view->bpc != 8) {
        /* 高位深没有可靠的复制-缩放路径，直接放弃这一帧（与
         * CPU 缩放器"高位深退回原尺寸"不同——这里连缓存都进不去，
         * 理论到不了：app 只在 8bit I420 的 AV1 上跑本管线）。 */
        return;
    }

    owner = (uint8_t *)malloc(yB + 2 * uvB);
    if (!owner) return;

    memcpy(owner, view->plane[0], yB);
    if (uvB) {
        memcpy(owner + yB, view->plane[1], uvB);
        memcpy(owner + yB + uvB, view->plane[2], uvB);
    }

    {
        LDVFrameView  out = *view;
        LDVResult     r;
        out.plane[0] = owner;
        out.plane[1] = uvB ? (owner + yB) : NULL;
        out.plane[2] = uvB ? (owner + yB + uvB) : NULL;
        out.index = 0;
        r = LDVFrameCachePushOwned(_cache, &out, owner, yB + 2 * uvB);
        if (r != LDV_OK) free(owner);
    }
}

/* ------------------------------------------------------------------------ */
/* GL                                                                        */
/* ------------------------------------------------------------------------ */

- (void)setupGL {
    _ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!_ctx || ![EAGLContext setCurrentContext:_ctx]) {
        _ctx = nil;
        _glBroken = 1;
        return;
    }

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_maxTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);     /* 源 stride 不保证 4 对齐 */

    glGenTextures(1, &_texY);
    glGenTextures(1, &_texU);
    glGenTextures(1, &_texV);
    glGenTextures(1, &_fboTex);
    glGenFramebuffers(1, &_fbo);

    _prog = [self buildProgram];
    _attrPos = glGetAttribLocation(_prog, "a_pos");
    _uVisY   = glGetUniformLocation(_prog, "u_visY");
    _uVisUV  = glGetUniformLocation(_prog, "u_visUV");
    if (!_prog || _attrPos < 0 || _uVisY < 0 || _uVisUV < 0) {
        _glBroken = 1;
        return;
    }
    glUseProgram(_prog);
    glUniform1i(glGetUniformLocation(_prog, "s_y"), 0);
    glUniform1i(glGetUniformLocation(_prog, "s_u"), 1);
    glUniform1i(glGetUniformLocation(_prog, "s_v"), 2);
    glEnableVertexAttribArray((GLuint)_attrPos);
}

- (GLuint)buildProgram {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint prog;
    GLint  ok = 0;
    char   log[256];

    glShaderSource(vs, 1, &ldv_es2_tc_vs, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        fprintf(stderr, "LDVEs2Tc: 顶点着色器编译失败: %s\n", log);
        return 0;
    }

    glShaderSource(fs, 1, &ldv_es2_tc_fs, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        fprintf(stderr, "LDVEs2Tc: 片元着色器编译失败: %s\n", log);
        return 0;
    }

    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "LDVEs2Tc: 程序链接失败: %s\n", log);
        return 0;
    }
    return prog;
}

/*
 * 源纹理分配（首帧或源尺寸变化时调用）。纹理宽 = stride 字节宽，
 * 上传时只分配、每帧 glTexSubImage2D 填充。
 */
- (BOOL)ensureSourceTexturesForView:(const LDVFrameView *)view {
    if (_srcW == view->width && _srcH == view->height &&
        _srcStrideY == view->stride[0] && _srcStrideUV == view->stride[1]) {
        return YES;
    }

    if ((GLint)view->stride[0] > _maxTex) {
        /* SGX535（上限 1024）装不下 720p 的 stride 1280 —— 直通兜底。 */
        fprintf(stderr, "LDVEs2Tc: 源宽 %d 超过 GL 上限 %d，转直通\n",
                (int)view->stride[0], (int)_maxTex);
        return NO;
    }

    glBindTexture(GL_TEXTURE_2D, _texY);
    ldv_es2_tc_tex_params();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 (GLsizei)view->stride[0], (GLsizei)view->height,
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

    {
        GLsizei uvH = (GLsizei)((view->height + 1) / 2);
        if ((GLint)view->stride[1] > _maxTex) return NO;
        glBindTexture(GL_TEXTURE_2D, _texU);
        ldv_es2_tc_tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     (GLsizei)view->stride[1], uvH,
                     0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, _texV);
        ldv_es2_tc_tex_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     (GLsizei)view->stride[1], uvH,
                     0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    }

    if (glGetError() != GL_NO_ERROR) return NO;

    _srcW = view->width;
    _srcH = view->height;
    _srcStrideY = view->stride[0];
    _srcStrideUV = view->stride[1];
    return YES;
}

/* 目标 FBO（RGBA）+ 回读缓冲。目标变化时重分配。 */
- (BOOL)ensureTargetFboW:(int)w h:(int)h2 {
    GLsizei W = (GLsizei)w, H = (GLsizei)h2;

    if (_fboW == w && _fboH == h2 && _readback) return YES;

    glBindTexture(GL_TEXTURE_2D, _fboTex);
    ldv_es2_tc_tex_params();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, _fboTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "LDVEs2Tc: FBO %dx%d 不完整，转直通\n", w, h2);
        return NO;
    }

    {
        uint8_t *nb = (uint8_t *)realloc(_readback, (size_t)W * H * 4);
        if (!nb) return NO;
        _readback = nb;
    }

    _fboW = w;
    _fboH = h2;
    return YES;
}

/* 一帧的完整转换：上传 → 绘制 → 回读 → 解包 → 推缓存。 */
- (void)transcodeSlot:(LDVEs2TcSlot *)slot dstW:(int)dstW dstH:(int)dstH {
    const LDVFrameView *v = &slot->view;

    if (![self ensureSourceTexturesForView:v] ||
        ![self ensureTargetFboW:dstW h:dstH]) {
        _glBroken = 1;
        [self pushSlotDirectly:slot];
        return;
    }

    /* —— 上传三平面（纹理宽 = stride，零拷贝老办法） —— */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _texY);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    (GLsizei)v->stride[0], (GLsizei)v->height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, v->plane[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, _texU);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    (GLsizei)v->stride[1], (GLsizei)((v->height + 1) / 2),
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, v->plane[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, _texV);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    (GLsizei)v->stride[1], (GLsizei)((v->height + 1) / 2),
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, v->plane[2]);

    /* —— 绘制 —— */
    {
        static const GLfloat quad[16] = {
            /* x, y, s, t —— t=0 在 clip y=-1（窗口底），readPixels
             * 第 0 行即源顶行，解包不翻转。 */
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
        };
        glViewport(0, 0, (GLsizei)dstW, (GLsizei)dstH);
        glUseProgram(_prog);
        glUniform1f(_uVisY,  (GLfloat)v->width / (GLfloat)v->stride[0]);
        glUniform1f(_uVisUV, (GLfloat)((v->width + 1) / 2)
                           / (GLfloat)v->stride[1]);
        glVertexAttribPointer(_attrPos, 4, GL_FLOAT, GL_FALSE, 0, quad);
        glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* —— 回读 —— */
    glReadPixels(0, 0, (GLsizei)dstW, (GLsizei)dstH,
                 GL_RGBA, GL_UNSIGNED_BYTE, _readback);

    /* —— 解包 RGBA → I420（整块 owner：Y | U | V） ——
     *
     * ★ 行 stride 必须 64 字节对齐（v8.21b）：Metal 显示端的 stride 纹理
     *   用 view->stride 当 bytesPerRow，AGX 驱动（iOS 9 validateStride-
     *   TextureParameters）要求 64 对齐，不对齐直接 abort（无法捕获）。
     *   dav1d 原生 stride 是 128 对齐所以从来不出事；这里输出紧凑
     *   stride=width（如 568）在 SE1 一建纹理就崩（.ips 实锤）。
     *   GLES 的"纹理宽 = stride"技法与 CPU 逐行路径都不要求对齐，
     *   pad 一点字节对它们无感。 */
    {
        size_t   yStride = (size_t)((dstW + 63) & ~63);
        size_t   uvStride = (size_t)(((dstW / 2) + 63) & ~63);
        size_t   ySize = yStride * (size_t)dstH;
        size_t   uSize = uvStride * (size_t)(dstH / 2);
        uint8_t *owner = (uint8_t *)malloc(ySize + 2 * uSize);
        if (!owner) {
            free(slot->owner);       /* 没内存转换就丢帧，绝不反压解码 */
            slot->owner = NULL;
            return;
        }

        {
            uint8_t *yP = owner;
            uint8_t *uP = owner + ySize;
            uint8_t *vP = uP + uSize;
            int      i, j;

            for (i = 0; i < dstH; i++) {
                const uint8_t *row = _readback + (size_t)i * dstW * 4;
                uint8_t       *yRow = yP + (size_t)i * yStride;
                for (j = 0; j < dstW; j++) {
                    yRow[j] = row[j * 4];
                }
            }
            for (i = 0; i < dstH / 2; i++) {
                /* 顶行（偶数行）取点，采样质量由 GL_LINEAR 保证 */
                const uint8_t *row = _readback + (size_t)(2 * i) * dstW * 4;
                uint8_t       *uRow = uP + (size_t)i * uvStride;
                uint8_t       *vRow = vP + (size_t)i * uvStride;
                for (j = 0; j < dstW / 2; j++) {
                    uRow[j] = row[(2 * j) * 4 + 1];
                    vRow[j] = row[(2 * j) * 4 + 2];
                }
            }
        }

        {
            LDVFrameView out = *v;
            LDVResult    r;
            out.width     = dstW;
            out.height    = dstH;
            out.plane[0]  = owner;
            out.plane[1]  = owner + ySize;
            out.plane[2]  = owner + ySize + uSize;
            out.stride[0] = (ptrdiff_t)yStride;
            out.stride[1] = (ptrdiff_t)uvStride;
            out.index     = 0;
            /* 缓存满 → 背压等待重试，不丢帧。显示节拍 = 缓存序号差 ×
             * 帧周期；丢帧会把序号压缩（时间上跳了 N 帧的相邻帧在
             * 缓存里序号只差 1），内容时间按丢帧倍率快进 —— v8.21
             * SE1 实测"倍速"的根源。等显示消费腾位（100ms 粒度，
             * _stopFlag 或缓存销毁会唤醒）。 */
            for (;;) {
                r = LDVFrameCachePushOwned(_cache, &out, owner,
                                           ySize + 2 * uSize);
                if (r == LDV_OK) break;
                if (_stopFlag || r != LDV_ERR_NEED_INPUT) {
                    free(owner);      /* 停止/其它错误：释放，停止路径绝不背压 */
                    break;
                }
                LDVFrameCacheWaitForSpace(_cache, 100);
            }
        }
    }

    /* 源帧（slot 的拷贝）用完释放 */
    free(slot->owner);
    slot->owner = NULL;
}

- (void)teardownGL {
    if (!_ctx) return;

    if (_prog) glDeleteProgram(_prog);
    glDeleteTextures(1, &_texY);
    glDeleteTextures(1, &_texU);
    glDeleteTextures(1, &_texV);
    glDeleteTextures(1, &_fboTex);
    glDeleteFramebuffers(1, &_fbo);
    _texY = _texU = _texV = _fboTex = _fbo = _prog = 0;

    free(_readback);
    _readback = NULL;

    [EAGLContext setCurrentContext:nil];
    _ctx = nil;
}

@end
