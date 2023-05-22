#ifndef SRMBUFFERPRIVATE_H
#define SRMBUFFERPRIVATE_H

#include "../SRMBuffer.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <gbm.h>
#include <linux/dma-buf.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SRMBufferTexture
{
    SRMDevice *device;
    EGLImage image;
    GLuint texture;
    UInt8 updated;
};

struct SRMBufferStruct
{
    // Common
    pthread_mutex_t mutex;
    enum SRM_BUFFER_SRC src;

    UInt32 caps;
    UInt32 planesCount;
    UInt32 format;
    UInt32 width;
    UInt32 height;
    UInt32 bpp;
    UInt32 pixelSize;
    UInt32 stride;
    UInt32 offset;
    UInt64 modifier;
    SRMCore *core;
    SRMList *textures;

    // GBM
    struct gbm_bo *bo;
    enum gbm_bo_flags flags;
    void *mapData;

    // DMA
    Int32 fd;
    void *map;
    struct dma_buf_sync sync;

    // Gles
    GLuint framebuffer;
    GLint glInternalFormat;
    GLint glFormat;
    GLint glType;
};

SRMBuffer *srmBufferCreate(SRMCore *core);
Int32 srmBufferGetDMAFDFromBO(SRMDevice *device, struct gbm_bo *bo);

#ifdef __cplusplus
}
#endif

#endif // SRMBUFFERPRIVATE_H
