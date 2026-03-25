#include "nvme_read.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spdk/env.h>
#include <spdk/nvme.h>

typedef struct {
    struct spdk_nvme_ctrlr *ctrlr;
} nvme_probe_ctx_t;

typedef struct {
    bool done;
    int rc;
} nvme_io_ctx_t;

static int write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t written = 0;
    while (written < count) {
        ssize_t n = write(fd, p + written, count - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static int read_small_text_file(const char *path, char *buf, size_t buf_len) {
    if (path == NULL || buf == NULL || buf_len == 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t n = read(fd, buf, buf_len - 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (n < 0) {
        return -1;
    }

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' ||
                     buf[n - 1] == '\t')) {
        --n;
    }
    buf[n] = '\0';
    return 0;
}

static bool is_pci_bdf(const char *s) {
    // 兼容 "0000:5e:00.0" 与 "5e:00.0"
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%x:%x:%x.%x", &a, &b, &c, &d) == 4) {
        return true;
    }
    if (sscanf(s, "%x:%x.%x", &b, &c, &d) == 3) {
        return true;
    }
    return false;
}

static int resolve_traddr_from_device_name(const char *device_name, char *traddr, size_t traddr_len) {
    if (device_name == NULL || traddr == NULL || traddr_len == 0) {
        errno = EINVAL;
        return -1;
    }

    if (is_pci_bdf(device_name)) {
        int n = snprintf(traddr, traddr_len, "%s", device_name);
        if (n < 0 || (size_t)n >= traddr_len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    if (strncmp(device_name, "/dev/", 5) != 0) {
        errno = EINVAL;
        return -1;
    }

    const char *base = device_name + 5;  // "nvme0n1" / "nvme0"
    if (strncmp(base, "nvme", 4) != 0) {
        errno = EINVAL;
        return -1;
    }

    char ctrl_name[64];
    size_t i = 0;
    for (; base[i] != '\0' && i < sizeof(ctrl_name) - 1; ++i) {
        if (base[i] == 'n' && i > 4) {
            break;
        }
        ctrl_name[i] = base[i];
    }
    ctrl_name[i] = '\0';
    if (strcmp(ctrl_name, "nvme") == 0) {
        errno = EINVAL;
        return -1;
    }

    char sysfs_addr_path[256];
    int n = snprintf(sysfs_addr_path, sizeof(sysfs_addr_path), "/sys/class/nvme/%s/address", ctrl_name);
    if (n < 0 || (size_t)n >= sizeof(sysfs_addr_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (read_small_text_file(sysfs_addr_path, traddr, traddr_len) != 0) {
        return -1;
    }
    if (!is_pci_bdf(traddr)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static bool probe_cb(void *cb_ctx,
                     const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts *opts) {
    (void)cb_ctx;
    (void)trid;
    (void)opts;
    return true;
}

static void attach_cb(void *cb_ctx,
                      const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts) {
    (void)trid;
    (void)opts;
    nvme_probe_ctx_t *ctx = (nvme_probe_ctx_t *)cb_ctx;
    if (ctx->ctrlr == NULL) {
        ctx->ctrlr = ctrlr;
        return;
    }
    spdk_nvme_detach(ctrlr);
}

static void io_complete(void *arg, const struct spdk_nvme_cpl *completion) {
    nvme_io_ctx_t *ctx = (nvme_io_ctx_t *)arg;
    ctx->rc = spdk_nvme_cpl_is_error(completion) ? -EIO : 0;
    ctx->done = true;
}

int nvme_read(const char *device_name,
              uint64_t backup_lba,
              uint64_t data_len,
              void *buffer,
              const char *filename) {
    (void)buffer;

    if (device_name == NULL || filename == NULL || data_len == 0) {
        errno = EINVAL;
        fprintf(stderr, "invalid argument: device_name/filename/data_len\n");
        return -1;
    }

    if (data_len % NVME_LBA_SIZE_BYTES != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "data_len must be %llu-byte aligned, got %llu\n",
                (unsigned long long)NVME_LBA_SIZE_BYTES, (unsigned long long)data_len);
        return -1;
    }

    if (NVME_READ_CHUNK_BYTES % NVME_LBA_SIZE_BYTES != 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "NVME_READ_CHUNK_BYTES must be %llu-byte aligned\n",
                (unsigned long long)NVME_LBA_SIZE_BYTES);
        return -1;
    }

    if (device_name[0] == '\0') {
        errno = EINVAL;
        fprintf(stderr, "device_name cannot be empty\n");
        return -1;
    }

    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "nvme_single_thread_read";
    if (spdk_env_init(&opts) < 0) {
        errno = EIO;
        fprintf(stderr, "spdk_env_init failed\n");
        return -1;
    }

    int ret = -1;
    struct spdk_nvme_qpair *qpair = NULL;
    void *chunk_buf = NULL;
    nvme_probe_ctx_t probe_ctx;
    memset(&probe_ctx, 0, sizeof(probe_ctx));

    char traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
    if (resolve_traddr_from_device_name(device_name, traddr, sizeof(traddr)) != 0) {
        fprintf(stderr,
                "invalid device_name %s, expected PCI BDF or /dev/nvme* with sysfs address\n",
                device_name);
        goto out;
    }

    struct spdk_nvme_transport_id trid;
    memset(&trid, 0, sizeof(trid));
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
    (void)snprintf(trid.traddr, sizeof(trid.traddr), "%s", traddr);
    if (spdk_nvme_probe(&trid, &probe_ctx, probe_cb, attach_cb, NULL) != 0 || probe_ctx.ctrlr == NULL) {
        errno = ENODEV;
        fprintf(stderr, "spdk_nvme_probe failed for %s (traddr=%s)\n", device_name, traddr);
        goto out;
    }

    struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(probe_ctx.ctrlr, 1);
    if (ns == NULL || !spdk_nvme_ns_is_active(ns)) {
        errno = ENODEV;
        fprintf(stderr, "active nsid=1 not found for %s\n", device_name);
        goto out;
    }

    qpair = spdk_nvme_ctrlr_alloc_io_qpair(probe_ctx.ctrlr, NULL, 0);
    if (qpair == NULL) {
        errno = ENOMEM;
        fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair failed\n");
        goto out;
    }

    chunk_buf = spdk_zmalloc(NVME_READ_CHUNK_BYTES, 4096, NULL,
                             SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    if (chunk_buf == NULL) {
        errno = ENOMEM;
        fprintf(stderr, "spdk_zmalloc failed\n");
        goto out;
    }

    uint64_t segment_count_u64 =
        (data_len + NVME_SPLIT_BYTES - 1ULL) / NVME_SPLIT_BYTES;
    if (segment_count_u64 == 0ULL) {
        errno = EINVAL;
        fprintf(stderr, "invalid segment count for data_len=%llu\n", (unsigned long long)data_len);
        goto out;
    }

    ret = 0;
    size_t segment_count = (size_t)segment_count_u64;
    for (size_t segment_index = 0; segment_index < segment_count; ++segment_index) {
        uint64_t seg_offset = (uint64_t)segment_index * NVME_SPLIT_BYTES;
        uint64_t seg_len = data_len - seg_offset;
        if (seg_len > NVME_SPLIT_BYTES) {
            seg_len = NVME_SPLIT_BYTES;
        }

        char output_name[4096];
        int name_len = snprintf(output_name, sizeof(output_name), "%s_part%04zu.bin",
                                filename, segment_index);
        if (name_len < 0 || (size_t)name_len >= sizeof(output_name)) {
            errno = ENAMETOOLONG;
            fprintf(stderr, "output filename too long for segment %zu\n", segment_index);
            ret = -1;
            break;
        }

        int out_fd = open(output_name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "open %s failed: %s\n", output_name, strerror(errno));
            ret = -1;
            break;
        }

        uint64_t offset = 0;
        while (offset < seg_len) {
            uint64_t remaining = seg_len - offset;
            uint64_t chunk_size = remaining > NVME_READ_CHUNK_BYTES ? NVME_READ_CHUNK_BYTES : remaining;
            uint32_t lba_count = (uint32_t)(chunk_size / NVME_LBA_SIZE_BYTES);
            // 仅使用 cwd14/15 backup LBA：从 backup 基址线性递增读取。
            uint64_t chunk_lba = backup_lba + ((seg_offset + offset) / NVME_LBA_SIZE_BYTES);

            nvme_io_ctx_t io_ctx;
            io_ctx.done = false;
            io_ctx.rc = 0;
            int rc = spdk_nvme_ns_cmd_read(ns, qpair, chunk_buf, chunk_lba,
                                           lba_count, io_complete, &io_ctx, 0);
            if (rc != 0) {
                errno = EIO;
                fprintf(stderr, "spdk_nvme_ns_cmd_read submit failed, lba=%llu blocks=%llu\n",
                        (unsigned long long)chunk_lba, (unsigned long long)lba_count);
                ret = -1;
                break;
            }

            while (!io_ctx.done) {
                rc = spdk_nvme_qpair_process_completions(qpair, 0);
                if (rc < 0) {
                    errno = EIO;
                    fprintf(stderr, "spdk_nvme_qpair_process_completions failed\n");
                    ret = -1;
                    break;
                }
            }
            if (ret != 0) {
                break;
            }

            if (io_ctx.rc != 0) {
                errno = EIO;
                fprintf(stderr, "SPDK read completion error, lba=%llu blocks=%llu\n",
                        (unsigned long long)chunk_lba, (unsigned long long)lba_count);
                ret = -1;
                break;
            }

            if (write_all(out_fd, chunk_buf, (size_t)chunk_size) != 0) {
                fprintf(stderr, "write failed for %s: %s\n", output_name, strerror(errno));
                ret = -1;
                break;
            }

            offset += chunk_size;
        }

        if (close(out_fd) != 0) {
            fprintf(stderr, "close %s failed: %s\n", output_name, strerror(errno));
            ret = -1;
        }

        if (ret != 0) {
            break;
        }
    }

out:
    if (chunk_buf != NULL) {
        spdk_free(chunk_buf);
    }
    if (qpair != NULL) {
        spdk_nvme_ctrlr_free_io_qpair(qpair);
    }
    if (probe_ctx.ctrlr != NULL) {
        spdk_nvme_detach(probe_ctx.ctrlr);
    }
    spdk_env_fini();
    return ret;
}
