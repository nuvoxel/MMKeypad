/* mmk-selftest camera -- one-frame V4L2 capture from /dev/video0 on the T3
 * (RK3188 / nt99141 CIF). Adapted from the proven bring-up program in
 * the T3 notes kept outside this repo.
 *
 * Standard V4L2_MEMORY_MMAP hits a QUERYBUF EINVAL on this ancient (3.0.36)
 * videobuf/soc_camera stack. The working path uses an ION-backed
 * V4L2_MEMORY_OVERLAY buffer: REQBUFS(OVERLAY) lets the driver claim its own
 * IPP-zoom scratch first, then we allocate a physically-contiguous buffer from
 * /dev/ion, hand its phys addr to QBUF as m.offset, STREAMON, DQBUF. The
 * capture blit runs through rk29-ipp (now loaded via kver-matching), so real
 * frames come back. v4l2_buffer ioctls MUST use a kernel-exact 68-byte struct +
 * hand-computed _IOC numbers -- musl's 64-bit time_t makes the header's
 * v4l2_buffer 80 bytes and bakes the wrong size into VIDIOC_QBUF/DQBUF.
 *
 * Writes the NV12 frame to /data/cam.nv12 (pull it and convert to PNG on the
 * host). Give the sensor ~30 frames of warmup for auto-exposure.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define CAM_OUT "/data/cam.nv12"

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

/* ---- ion.h, transcribed from the vendor kernel (drivers/gpu/ion) ---- */
enum ion_heap_ids { ION_NOR_HEAP_ID = 0, ION_CMA_HEAP_ID = 1, ION_CAM_ID = 17 };
struct ion_handle;
struct ion_allocation_data { size_t len; size_t align; unsigned int flags; struct ion_handle *handle; };
struct ion_fd_data { struct ion_handle *handle; int fd; };
struct ion_handle_data { struct ion_handle *handle; };
struct ion_phys_data { struct ion_handle *handle; unsigned long phys; unsigned long size; };
#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC       _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_FREE        _IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data)
#define ION_IOC_MAP         _IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data)
#define ION_CUSTOM_GET_PHYS _IOWR(ION_IOC_MAGIC, 7, struct ion_phys_data)
struct ion_cacheop_data { unsigned int type; struct ion_handle *handle; void *virt; };
#define ION_CACHE_INV   2
#define ION_CUSTOM_CACHE_OP _IOWR(ION_IOC_MAGIC, 8, struct ion_cacheop_data)

/* ---- kernel-EXACT struct v4l2_buffer (68 bytes, 32-bit timeval) ---- */
struct k_timeval  { long tv_sec; long tv_usec; };
struct k_timecode { unsigned int type, flags; unsigned char frames, seconds, minutes, hours, userbits[4]; };
struct k_v4l2_buffer {
    unsigned int index, type, bytesused, flags, field;
    struct k_timeval  timestamp;
    struct k_timecode timecode;
    unsigned int sequence, memory;
    union { unsigned int offset; unsigned long userptr; void *planes; } m;
    unsigned int length, input, reserved;
};
#define K_VIDIOC_QBUF  _IOWR('V', 15, struct k_v4l2_buffer)
#define K_VIDIOC_DQBUF _IOWR('V', 17, struct k_v4l2_buffer)

int mmk_selftest_camera(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";
    const int W = 1280, H = 720;   /* nt99141 native mode */
    printf("=== mmk-selftest camera: capture one frame from %s ===\n", dev);

    int vfd = open(dev, O_RDWR);
    if (vfd < 0) { printf("open(%s): %s\n", dev, strerror(errno)); return 1; }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = W; fmt.fmt.pix.height = H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) { printf("S_FMT: %s\n", strerror(errno)); return 1; }
    unsigned sizeimage = fmt.fmt.pix.sizeimage;
    size_t alloc_len = ((sizeimage + 4095u) & ~4095u);
    if (alloc_len < 4u * 1024 * 1024) alloc_len = 4u * 1024 * 1024;
    printf("S_FMT OK: NV12 %ux%u sizeimage=%u alloc_len=%zu\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height, sizeimage, alloc_len);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_OVERLAY;
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) { printf("REQBUFS(OVERLAY): %s\n", strerror(errno)); return 1; }
    printf("REQBUFS(OVERLAY) OK, count=%u\n", req.count);

    int ionfd = open("/dev/ion", O_RDWR);
    if (ionfd < 0) { printf("open(/dev/ion): %s\n", strerror(errno)); return 1; }

    struct ion_allocation_data ad;
    unsigned int cand[]  = { 1u << ION_CAM_ID, 1u << ION_CMA_HEAP_ID, 1u << ION_NOR_HEAP_ID, 0xffffffffu };
    const char  *names[] = { "CAM(17)", "CMA(1)", "NOR/system(0)", "any" };
    int alloc_ok = 0;
    for (size_t hi = 0; hi < sizeof(cand) / sizeof(cand[0]); hi++) {
        memset(&ad, 0, sizeof(ad));
        ad.len = alloc_len; ad.align = 4096; ad.flags = cand[hi];
        if (xioctl(ionfd, ION_IOC_ALLOC, &ad) < 0) {
            printf("ION_IOC_ALLOC(%s): %s\n", names[hi], strerror(errno)); continue;
        }
        printf("ION_IOC_ALLOC(%s) OK\n", names[hi]); alloc_ok = 1; break;
    }
    if (!alloc_ok) { printf("no ion heap accepted the allocation\n"); return 1; }

    struct ion_fd_data fdd;
    memset(&fdd, 0, sizeof(fdd)); fdd.handle = ad.handle;
    if (xioctl(ionfd, ION_IOC_MAP, &fdd) < 0) { printf("ION_IOC_MAP: %s\n", strerror(errno)); return 1; }

    void *va = mmap(NULL, alloc_len, PROT_READ | PROT_WRITE, MAP_SHARED, fdd.fd, 0);
    if (va == MAP_FAILED) { printf("mmap(ion): %s\n", strerror(errno)); return 1; }
    memset(va, 0xAA, alloc_len);   /* sentinel: detect whether the camera writes */

    struct ion_phys_data pd;
    memset(&pd, 0, sizeof(pd)); pd.handle = ad.handle;
    if (xioctl(ionfd, ION_CUSTOM_GET_PHYS, &pd) < 0) { printf("GET_PHYS: %s\n", strerror(errno)); return 1; }
    printf("phys=0x%lx size=0x%lx\n", pd.phys, pd.size);

    struct k_v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_OVERLAY;
    buf.index = 0; buf.m.offset = (unsigned int)pd.phys; buf.length = (unsigned)alloc_len;
    if (xioctl(vfd, K_VIDIOC_QBUF, &buf) < 0) { printf("QBUF(OVERLAY): %s\n", strerror(errno)); return 1; }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) { printf("STREAMON: %s\n", strerror(errno)); return 1; }
    printf("STREAMON OK, warming up ...\n");

    int got = 0;
    for (int tries = 0; tries < 30; tries++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_OVERLAY;
        if (xioctl(vfd, K_VIDIOC_DQBUF, &buf) < 0) { printf("DQBUF (try %d): %s\n", tries, strerror(errno)); break; }
        struct ion_cacheop_data co; memset(&co, 0, sizeof co);
        co.type = ION_CACHE_INV; co.handle = ad.handle; co.virt = va;
        xioctl(ionfd, ION_CUSTOM_CACHE_OP, &co);
        got = 1;
        struct k_v4l2_buffer rq = buf;
        rq.m.offset = (unsigned int)pd.phys; rq.length = (unsigned)alloc_len;
        if (xioctl(vfd, K_VIDIOC_QBUF, &rq) < 0) { printf("requeue: %s\n", strerror(errno)); break; }
    }

    int rc = 1;
    if (got) {
        uint8_t *px = (uint8_t *)va;
        size_t len = buf.bytesused ? buf.bytesused : sizeimage;
        unsigned mn = 255, mx = 0; uint64_t sum = 0;
        for (size_t i = 0; i < len; i++) { uint8_t v = px[i]; if (v < mn) mn = v; if (v > mx) mx = v; sum += v; }
        int real = (mx - mn) > 8;
        printf("CAPTURED: %zu bytes, luma min=%u max=%u mean=%.1f -> %s\n",
               len, mn, mx, len ? (double)sum / len : 0,
               real ? "REAL IMAGE DATA" : "FLAT (no signal / sensor asleep)");
        int of = open(CAM_OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (of >= 0) { write(of, px, len); close(of); printf("wrote %s (%zu bytes, NV12 %dx%d)\n", CAM_OUT, len, W, H); }
        rc = real ? 0 : 1;
    } else {
        printf("VERDICT: FAIL -- no frame captured\n");
    }

    xioctl(vfd, VIDIOC_STREAMOFF, &type);
    munmap(va, alloc_len); close(fdd.fd);
    struct ion_handle_data hd; memset(&hd, 0, sizeof(hd)); hd.handle = ad.handle;
    xioctl(ionfd, ION_IOC_FREE, &hd);
    close(ionfd); close(vfd);
    return rc;
}
