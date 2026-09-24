# NDS API 文档

## 0. 文档说明

- **NDS 依据**：本文件以 `file_p2p/nds_api.h` 为准（`NDS_API_VERSION == 1`），语义与实现细节参考 `file_p2p/nds_api.c`。
- **GDS 依据**：NVIDIA cuFile / GPUDirect Storage 最新开发包中的 `cufile.h`（`libcufile 1.19.1.55`，对应 CUDA 13.x 的 `libcufile-dev` / `libcufile-devel`）。
- **定位**：NDS 是面向 NPU HBM ↔ NVMe P2P 的、与 GDS 对齐的异步用户态 API。
- **约定**：所有函数成功返回 `0` 或正计数，失败返回 `-errno`；I/O 仅异步；目标为已打开的文件或块设备 fd（不接受路径）；偏移/长度/缓冲区地址必须按 512B 扇区对齐；`reserved` 字段必须为 0。

本文分三部分：

1. **接口总览**：NDS 全量接口清单，并与 GDS 逐项对位。
2. **接口对比**：按功能域对 GDS 与 NDS 做 1 对 1 详细对比。
3. **接口使用**：调用流程与 C/Python 示例。

---

# 第一部分 接口总览

## 1.1 调用模型

NDS 采用「进程级初始化 + 文件系统注册 + 内存注册 + 每队列 I/O 上下文 + 提交/收割」的模型，与 Linux AIO 的 `io_submit` / `io_getevents` 形状一致：

```
nds_init()               进程级初始化（打开内部设备 fd）
  └─ nds_register_fs()   注册文件系统（可反复调用，动态增长）
  └─ nds_register_mem()  固定 HBM/CMB 窗口（可选，热路径复用）
  └─ nds_io_new_ctx()    创建一个 I/O 上下文（独立 fd）
        ├─ nds_io_submit()      提交 N 个控制块（异步）
        └─ nds_io_getevents()   收割完成事件
  └─ nds_io_destroy_ctx() 销毁上下文（完成未决 I/O）
  └─ nds_unregister_mem()
  └─ nds_exit()
```

## 1.2 NDS 接口与 GDS 接口 1 对 1 总览

下表给出 NDS 详细接口原型，并与 GDS 逐项对位。

| 分类 | 功能 | GDS API（cufile.h） | NDS API（nds_api.h） | 对等 |
| --- | --- | --- | --- | --- |
| 生命周期 | 初始化库 | `CUfileError_t cuFileDriverOpen(void)` | `int nds_init(struct nds_init_param *param)` | 是 |
| 生命周期 | 关闭库 | `CUfileError_t cuFileDriverClose(void)` | `int nds_exit(void)` | 是 |
| 生命周期 | 驱动使用计数 | `long cuFileUseCount(void)` | 无 | 否 |
| 目标注册 | 注册文件句柄 / 文件系统句柄 | `CUfileError_t cuFileHandleRegister(CUfileHandle_t *fh, CUfileDescr_t *descr)` | `int nds_register_fs(const struct nds_fs_desc *desc)` | 否 |
| 目标注册 | 注销文件句柄 / 文件系统句柄 | `void cuFileHandleDeregister(CUfileHandle_t fh)` | `int nds_unregister_fs(const struct nds_fs_desc *desc)` | 否 |
| 内存注册 | 注册设备内存 | `CUfileError_t cuFileBufRegister(const void *bufPtr_base, size_t length, int flags)` | `int nds_register_mem(void *addr, uint64_t size, int flags)` | 是 |
| 内存注册 | 注销设备内存 | `CUfileError_t cuFileBufDeregister(const void *bufPtr_base)` | `int nds_unregister_mem(void *addr, uint64_t size, int flags)` | 是 |
| I/O 上下文 | 创建批 / 上下文 | `CUfileError_t cuFileBatchIOSetUp(CUfileBatchHandle_t *batch_idp, unsigned nr)` | `int nds_io_new_ctx(const struct nds_io_ctx_param *param, struct nds_io_ctx **ctx)` | 是 |
| I/O 上下文 | 销毁批 / 上下文 | `void cuFileBatchIODestroy(CUfileBatchHandle_t batch_idp)` | `int nds_io_destroy_ctx(struct nds_io_ctx *ctx)` | 是 |
| 异步 I/O | 提交异步 I/O | `CUfileError_t cuFileBatchIOSubmit(CUfileBatchHandle_t batch_idp, unsigned nr, CUfileIOParams_t *iocbp, unsigned int flags)` | `int nds_io_submit(struct nds_io_ctx *ctx, int nr, const struct nds_io_cb *iocb)` | 是 |
| 异步 I/O | 收割完成 | `CUfileError_t cuFileBatchIOGetStatus(CUfileBatchHandle_t batch_idp, unsigned min_nr, unsigned *nr, CUfileIOEvents_t *iocbp, struct timespec *timeout)` | `int nds_io_getevents(struct nds_io_ctx *ctx, int min_nr, int nr, struct nds_io_event *events, struct timespec *timeout)` | 是 |
| 异步 I/O | 取消批 | `CUfileError_t cuFileBatchIOCancel(CUfileBatchHandle_t batch_idp)` | 无（销毁上下文时完成未决 I/O） | 否 |
| 同步 I/O | 同步读 | `ssize_t cuFileRead(CUfileHandle_t fh, void *bufPtr_base, size_t size, off_t file_offset, off_t bufPtr_offset)` | 无（异步-only，用 submit + getevents 组合） | 否 |
| 同步 I/O | 同步写 | `ssize_t cuFileWrite(CUfileHandle_t fh, const void *bufPtr_base, size_t size, off_t file_offset, off_t bufPtr_offset)` | 无（同上） | 否 |
| 向量 I/O | 分散/聚集读 | `ssize_t cuFileReadv(CUfileHandle_t fh, const CUfileIOVec_t *iov, size_t iovcnt, off_t file_offset, unsigned flags)` | `nds_io_submit` + 多 `nds_io_vec` | 是 |
| 向量 I/O | 分散/聚集写 | `ssize_t cuFileWritev(CUfileHandle_t fh, const CUfileIOVec_t *iov, size_t iovcnt, off_t file_offset, unsigned flags)` | `nds_io_submit` + 多 `nds_io_vec` | 是 |
| 流式异步 | CUDA 流异步读 | `CUfileError_t cuFileReadAsync(CUfileHandle_t fh, void *bufPtr_base, size_t *size_p, off_t *file_offset_p, off_t *bufPtr_offset_p, ssize_t *bytes_read_p, CUstream stream)` | 无（NDS 用完成队列，不绑定 CUDA 流） | 否 |
| 流式异步 | CUDA 流异步写 | `CUfileError_t cuFileWriteAsync(CUfileHandle_t fh, void *bufPtr_base, size_t *size_p, off_t *file_offset_p, off_t *bufPtr_offset_p, ssize_t *bytes_written_p, CUstream stream)` | 无 | 否 |
| 流式异步 | 注册/注销流 | `CUfileError_t cuFileStreamRegister(CUstream stream, unsigned flags)` / `CUfileError_t cuFileStreamDeregister(CUstream stream)` | 无 | 否 |
| 配置 | 获取驱动属性 | `CUfileError_t cuFileDriverGetProperties(CUfileDrvProps_t *props)` | 无（仅 `max_io_cnt` 提示） | 否 |
| 配置 | 轮询/缓存/固定内存 | `cuFileDriverSetPollMode` / `cuFileDriverSetMaxDirectIOSize` / `cuFileDriverSetMaxCacheSize` / `cuFileDriverSetMaxPinnedMemSize` | 无 | 否 |
| 配置 | 参数 get/set | `cuFileGetParameter*` / `cuFileSetParameter*` | 无 | 否 |
| 配置 | P2P flags | `CUfileError_t cuFileDriverGetP2PFlags(CUfileDriverStatusFlags_t, CUfileP2PFlags_t *)` / `cuFileDriverSetP2PFlags(...)` | 无 | 否 |
| 信息 | 库版本 | `CUfileError_t cuFileGetVersion(int *version)` | `nds_init_param.version`（输出） | 是 |
| 信息 | 导出 PCIe 拓扑 | `CUfileError_t cuFileExportPCIeTopology(const char *filename)` | 无 | 否 |
| 信息 | 查询 BAR 大小 | `CUfileError_t cuFileGetBARSizeInKB(int gpuIndex, size_t *barSize)` | 无 | 否 |
| 统计 | 统计开关与读取 | `cuFileSetStatsLevel` / `cuFileGetStatsLevel` / `cuFileStatsStart` / `cuFileStatsStop` / `cuFileStatsReset` / `cuFileGetStatsL1` / `cuFileGetStatsL2` / `cuFileGetStatsL3` | 无 | 否 |

结论：NDS 只覆盖 GDS 的**数据面核心子集**（初始化/退出、目标与内存注册、上下文、异步提交与完成），并针对 NPU HBM↔NVMe P2P 增加了 GDS 没有的**文件系统注册**（`nds_register_fs`）。GDS 的同步、CUDA 流、配置、统计等外围接口 NDS v1 不提供。

---

# 第二部分 接口对比

## 2.1 初始化与退出

| 项目 | GDS | NDS |
| --- | --- | --- |
| 原型 | `CUfileError_t cuFileDriverOpen(void);`<br>`CUfileError_t cuFileDriverClose(void);` | `int nds_init(struct nds_init_param *param);`<br>`int nds_exit(void);` |
| 参数 | 无 | `param` 承载 `flags`、`reserved[4]`，并**输出** `version` |
| 返回值 | `CUfileError_t`（`err` + `cu_err`） | `0` / `-errno` |
| 版本协商 | 运行时 `cuFileGetVersion` | `nds_init` 成功后 `param->version == NDS_API_VERSION` |
| 线程安全 | 库内部管理 | **非线程安全、不可重入**；须单线程调用一次，且不得与 `nds_exit` 并发 |

**数据结构（`nds_init_param`）与常量（`NDS_API_VERSION`）**

```c
#define NDS_API_VERSION 1u      /* 对应 GDS 的 cuFileGetVersion 版本信息 */

struct nds_init_param {
    uint32_t flags;         /* v1 必须为 0 */
    uint64_t reserved[4];   /* 必须为 0 */
    uint32_t version;       /* 输出：成功时为 NDS_API_VERSION */
};
```

- `NDS_API_VERSION` 是 NDS 对 GDS `cuFileGetVersion` 的对应物，但通过 `nds_init` 输出而非独立函数。
- `flags` / `reserved` 是 NDS 独有校验：任何非零值返回 `-EINVAL`。

**对应关系**：`cuFileDriverOpen` ↔ `nds_init`，`cuFileDriverClose` ↔ `nds_exit`。差异在于 NDS 把「版本检查」放进初始化输出参数。

## 2.2 目标注册（文件/文件系统）

| 项目 | GDS | NDS |
| --- | --- | --- |
| 原型 | `CUfileError_t cuFileHandleRegister(CUfileHandle_t *fh, CUfileDescr_t *descr);`<br>`void cuFileHandleDeregister(CUfileHandle_t fh);` | `int nds_register_fs(const struct nds_fs_desc *desc);`<br>`int nds_unregister_fs(const struct nds_fs_desc *desc);` |
| 注册对象 | 单个已打开的**文件句柄** | 一组描述**文件系统或者块设备**的 fd（普通文件或块设备）。一个文件系统或者块设备只需要注册一次，但是相同的文件系统或者块设备也可以同时被多个进程重复注册 |
| 前置要求 | 文件必须以 `O_DIRECT` 打开 | 无 `O_DIRECT` 前置 |
| 产出 | 不透明 `CUfileHandle_t`，后续 I/O 复用它 | **无句柄产出**；后续 I/O 每次在 `nds_io_cb.obj.fd` 里直接携带对应文件系统的文件 fd 或者块设备的 fd |
| 动态增长 | 每次注册一个文件 | 可反复调用 `nds_register_fs` 增量注册；多 fd 按数组顺序处理 |
| 注销语义 | 释放该文件句柄 | 移除该 fd 对应的一个文件系统引用；只要还有其他引用，注册的文件系统仍有效 |
| 线程安全 | 库内部管理 | 初始化后并发 `nds_register_fs` 是线程安全的 |

**数据结构（`nds_fs_desc`）**

```c
struct nds_fs_desc {
    int32_t *fs_fd;       /* 文件系统或块设备的 fd 数组（普通文件或块设备） */
    uint32_t fs_fd_cnt;   /* v1 要求 >= 1 */
    uint32_t reserved;    /* 必须为 0 */
};
```

- GDS 的 `CUfileDescr_t` 描述「要注册的文件/句柄」；NDS 的 `nds_fs_desc` 描述「要注册的文件系统 fd」，二者位置对应但语义不同。

**对应关系**：`cuFileHandleRegister` ↔ `nds_register_fs`，`cuFileHandleDeregister` ↔ `nds_unregister_fs`。本质区别是 GDS 注册「文件句柄」，NDS 注册「文件系统」，NDS 的 I/O 目标通过 fd 每次显式给出，不存在 `CUfileHandle_t` 概念。

## 2.3 内存注册

| 项目 | GDS | NDS |
| --- | --- | --- |
| 原型 | `CUfileError_t cuFileBufRegister(const void *bufPtr_base, size_t length, int flags);`<br>`CUfileError_t cuFileBufDeregister(const void *bufPtr_base);` | `int nds_register_mem(void *addr, uint64_t size, int flags);`<br>`int nds_unregister_mem(void *addr, uint64_t size, int flags);` |
| 注册对象 | cudaMalloc 设备内存或主机内存 | HBM（或 CMB）VA 窗口 |
| 句柄 | 对调用者不可见 | 对调用者不可见；NDS 不对外暴露句柄 |
| 注销方式 | 按 `bufPtr_base` 地址 | 按**基地址 + 精确 size**（size 不匹配返回 `-EINVAL`） |
| flags | `CU_FILE_RDMA_REGISTER` 等 | 必须为 `0`（见下方 `NDS_IO_F_*`） |
| 失败处理 | 返回错误码 | 返回错误码 |
| 使用方式 | I/O 时传已注册指针 | I/O 时设 `NDS_IO_F_REGISTERED_MEM`，表示是已注册指针 |

**标志位（`NDS_IO_F_*`）**

```c
#define NDS_IO_F_REGISTERED_MEM (1u << 0)
```

- `NDS_IO_F_REGISTERED_MEM`：作为每个 `nds_io_cb.rw_flags` 的显式内存模式开关，而非注册调用的参数。

**对应关系**：`cuFileBufRegister` ↔ `nds_register_mem`，`cuFileBufDeregister` ↔ `nds_unregister_mem`。

## 2.4 I/O 上下文

| 项目 | GDS | NDS |
| --- | --- | --- |
| 原型 | `CUfileError_t cuFileBatchIOSetUp(CUfileBatchHandle_t *batch_idp, unsigned nr);`<br>`void cuFileBatchIODestroy(CUfileBatchHandle_t batch_idp);` | `int nds_io_new_ctx(const struct nds_io_ctx_param *param, struct nds_io_ctx **ctx);`<br>`int nds_io_destroy_ctx(struct nds_io_ctx *ctx);` |
| 容量参数 | `nr`（批大小） | `max_io_cnt`（`1..NDS_IO_MAX_IO_CNT`，v1 仅作队列提示，不强制） |
| 句柄 | `CUfileBatchHandle_t` | 不透明 `struct nds_io_ctx *` |
| 销毁语义 | 释放批资源 | **完成未决 I/O 后释放**；`destroy` 与 `getevents` 不可并发；二次销毁/使用已销毁对象为 UB |
| 资源隔离 | 各批相互隔离 | 各 ctx 相互隔离 |

**数据结构与常量（`nds_io_ctx_param`、`nds_io_ctx`、`NDS_IO_MAX_IO_CNT`）**

```c
#define NDS_IO_MAX_IO_CNT 1024u   /* 单次 submit 上限，也是 ctx 队列提示上限 */

struct nds_io_ctx;                /* 不透明，前向声明 */

struct nds_io_ctx_param {
    uint32_t max_io_cnt;   /* 取值 1..NDS_IO_MAX_IO_CNT */
    uint32_t flags;        /* v1 必须为 0 */
    uint64_t reserved;     /* 必须为 0 */
};
```

- `max_io_cnt` 对应 GDS `cuFileBatchIOSetUp` 的 `nr`，但在 v1 仅作队列大小提示，库不强制在途预算。
- `struct nds_io_ctx` 在公开头文件中只前向声明，对应 GDS 的不透明 `CUfileBatchHandle_t`。

**对应关系**：`cuFileBatchIOSetUp` ↔ `nds_io_new_ctx`，`cuFileBatchIODestroy` ↔ `nds_io_destroy_ctx`。

## 2.5 异步提交与完成

| 项目 | GDS | NDS |
| --- | --- | --- |
| 提交原型 | `CUfileError_t cuFileBatchIOSubmit(CUfileBatchHandle_t batch_idp, unsigned nr, CUfileIOParams_t *iocbp, unsigned int flags);` | `int nds_io_submit(struct nds_io_ctx *ctx, int nr, const struct nds_io_cb *iocb);` |
| 收割原型 | `CUfileError_t cuFileBatchIOGetStatus(CUfileBatchHandle_t batch_idp, unsigned min_nr, unsigned* nr, CUfileIOEvents_t *iocbp, struct timespec* timeout);` | `int nds_io_getevents(struct nds_io_ctx *ctx, int min_nr, int nr, struct nds_io_event *events, struct timespec *timeout);` |
| 提交返回 | `CUfileError_t`，实际完成由事件给出 | 返回**已接受的数量**（正）；批量中途失败返回已接受数量；一个都没接受返回 `-errno`（fail-stop，类似 Linux `io_submit`） |
| 完成返回 | 通过 `nr` 出参 + `CUfileIOEvents_t` | 返回**填充的事件数**（`>= 0`）；失败 `-errno` |
| 完成状态 | `CUfileStatus_t`（WAITING/PENDING/COMPLETE/FAILED/…）+ `ret` | `res`：成功为 `0`，失败为负 errno（不是字节数） |
| cookie | `CUfileIOParams_t.cookie` | `nds_io_cb.user_data`，在事件中回显 |
| 超时 | `struct timespec* timeout` | `struct timespec* timeout`；`NULL` 永久等待，`{0,0}` 非阻塞 |
| 空队列行为 | 按 timeout 语义 | 无在途 I/O 时**立即返回 0**（即使 `min_nr > 0`） |
| 取消 | `cuFileBatchIOCancel` | 无独立取消；销毁上下文时完成未决 I/O |

**数据结构与常量（`nds_io_obj`、`nds_io_vec`、`nds_io_cb`、`nds_io_event`、操作码、`NDS_IO_MAX_IOV`）**

```c
#define NDS_IO_MAX_IOV 65536u    /* 单个控制块的 iov 数量上限 */

#define NDS_IO_OP_PREAD  0u      /* 对应 GDS CUFILE_READ */
#define NDS_IO_OP_PWRITE 1u      /* 对应 GDS CUFILE_WRITE */

struct nds_io_obj {
    uint64_t reserved;           /* 必须为 0 */
    union {
        uint64_t _data;
        int32_t fd;              /* 打开的文件 / 块设备 fd */
    };
};

struct nds_io_vec {
    uint64_t buf_addr;
    uint32_t buf_len;
    uint32_t reserved;           /* 必须为 0 */
};

struct nds_io_cb {
    uint32_t opcode;             /* NDS_IO_OP_* */
    uint32_t rw_flags;           /* NDS_IO_F_* 位掩码 */
    struct nds_io_obj obj;
    uint64_t offset;             /* 目标对象内的字节偏移 */
    uint64_t user_data;          /* 回显在事件中的 cookie */
    const struct nds_io_vec *iov;
    uint32_t iov_cnt;
    int32_t host_pid;            /* 一次性缓冲区属主；注册内存时必须为 0 */
    uint64_t reserved[2];        /* 必须为 0 */
};

struct nds_io_event {
    uint64_t user_data;
    int64_t res;                 /* 0 = 成功；负值 = -errno */
    uint64_t reserved[4];        /* 必须为 0 */
};
```

- `NDS_IO_OP_PREAD` / `NDS_IO_OP_PWRITE` 对应 GDS `CUFILE_READ` / `CUFILE_WRITE`，但每个控制块天然接受 1..`NDS_IO_MAX_IOV` 个 `nds_io_vec`，因此同时覆盖 GDS 的标量与 `cuFileReadv` / `cuFileWritev`，无需单独的 PREADV/PWRITEV 操作码。
- `nds_io_vec` 与 GDS `CUfileIOVec_t`（`base` / `len`）字段对位，但 NDS 额外要求 `reserved == 0`。

**控制块字段对比**

| 概念 | GDS `CUfileIOParams_t` | NDS `nds_io_cb` |
| --- | --- | --- |
| 模式 | `mode`（`CUFILE_BATCH`，必须为首字段） | 无（仅一种异步模式） |
| 目标 | `fh`（已注册句柄） | `obj.fd`（每次携带 fd） |
| 操作 | `opcode`（`CUFILE_READ` / `CUFILE_WRITE`） | `opcode`（`NDS_IO_OP_PREAD` / `NDS_IO_OP_PWRITE`） |
| 缓冲 | `devPtr_base` + `devPtr_offset` + `size`（单段） | `iov` + `iov_cnt`（多段，天然支持 SG） |
| 文件偏移 | `file_offset` | `offset` |
| cookie | `cookie` | `user_data` |
| 内存模式 | 隐式（是否注册过） | 显式 `rw_flags & NDS_IO_F_REGISTERED_MEM` |
| 缓冲区属主 | 无 | `host_pid`（一次性内存时指定属主，注册内存时必须为 0） |

**完成结构对比**

| GDS `CUfileIOEvents_t` | NDS `nds_io_event` |
| --- | --- |
| `void *cookie` | `uint64_t user_data` |
| `CUfileStatus_t status` | 无（用 `res` 表达） |
| `size_t ret`（负错误或已传输字节） | `int64_t res`（`0` 或负 errno） |
| 无 | `uint64_t reserved[4]`（必须为 0） |

**对应关系**：`cuFileBatchIOSubmit` ↔ `nds_io_submit`，`cuFileBatchIOGetStatus` ↔ `nds_io_getevents`。

## 2.6 同步与向量 I/O

| 项目 | GDS | NDS |
| --- | --- | --- |
| 同步读 | `ssize_t cuFileRead(CUfileHandle_t fh, void *bufPtr_base, size_t size, off_t file_offset, off_t bufPtr_offset);` | 无。用 `nds_io_submit` + `nds_io_getevents` 组合 |
| 同步写 | `ssize_t cuFileWrite(...);` | 无（同上） |
| 分散读 | `ssize_t cuFileReadv(CUfileHandle_t fh, const CUfileIOVec_t *iov, size_t iovcnt, off_t file_offset, unsigned flags);` | `nds_io_submit` 中一个 `nds_io_cb` 携带多段 `nds_io_vec` |
| 分散写 | `ssize_t cuFileWritev(...);` | 同上 |
| 流式异步 | `cuFileReadAsync` / `cuFileWriteAsync`（绑定 `CUstream`） | 无（用完成队列，不绑定 CUDA 流） |
| 流注册 | `cuFileStreamRegister` / `cuFileStreamDeregister` | 无 |

**对应关系**：NDS 的 `NDS_IO_OP_PREAD` / `NDS_IO_OP_PWRITE` 本身接受 1..`NDS_IO_MAX_IOV` 个向量，因此**同时覆盖 GDS 的标量与向量接口**，无需单独的 PREADV/PWRITEV 操作码。同步与 CUDA 流接口在 NDS v1 中有意不提供。

---

# 第三部分 接口使用

## 3.1 典型流程

1. 用 `aclrtMalloc` 等 NPU 接口分配 HBM 缓冲区。
2. `nds_init(&ip)` 初始化，断言 `ip.version == NDS_API_VERSION`。
3. 每有一个可用文件系统，调用 `nds_register_fs(&fs)` 注册其文件系统；后续可继续增量注册。
4. （可选）对热路径缓冲区调用 `nds_register_mem(addr, size, 0)`，后续 I/O 置 `NDS_IO_F_REGISTERED_MEM`。
5. `nds_io_new_ctx(&(struct nds_io_ctx_param){ .max_io_cnt = N }, &ctx)` 创建 I/O 上下文。
6. 填充 `nds_io_cb`，`nds_io_submit(ctx, nr, &cb)` 提交。
7. `nds_io_getevents(ctx, min_nr, nr, events, timeout)` 收割完成事件。
8. 销毁：`nds_io_destroy_ctx(ctx)` → `nds_unregister_mem(...)` → `nds_exit()`。

## 3.2 C 使用示例

```c
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include "acl/acl.h"
#include "nds_api.h"

#define BUF_SIZE (1u << 20)

int main(void)
{
    struct nds_init_param ip = { 0 };
    struct nds_io_ctx *ctx;
    struct nds_io_event ev;
    struct nds_io_vec iov;
    struct nds_io_cb cb;
    void *hbm = NULL;
    int fd, ret;

    /* 0. 初始化 NPU，并用 aclrtMalloc 分配 HBM */
    aclInit(NULL);
    aclrtSetDevice(0);
    aclrtMalloc(&hbm, BUF_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);

    /* 1. 打开目标文件；同一个 fd 也可用作文件系统注册 */
    fd = open("/mnt/nvme/data.bin", O_RDWR | O_DIRECT);
    assert(fd >= 0);

    /* 2. 初始化并注册文件系统 */
    ret = nds_init(&ip);
    assert(ret == 0 && ip.version == NDS_API_VERSION);

    int fds[] = { fd };
    struct nds_fs_desc fs = { .fs_fd = fds, .fs_fd_cnt = 1 };
    assert(nds_register_fs(&fs) == 0);

    /* 3. 创建 I/O 上下文 */
    assert(nds_io_new_ctx(&(struct nds_io_ctx_param){ .max_io_cnt = 64 },
                          &ctx) == 0);

    /* 4. 用 aclrtMalloc 得到的 HBM 地址准备 I/O 向量 */
    iov.buf_addr = (uint64_t)(uintptr_t)hbm;
    iov.buf_len  = BUF_SIZE;
    iov.reserved = 0;

    /* 5. 提交一次 PREAD */
    memset(&cb, 0, sizeof(cb));
    cb.opcode    = NDS_IO_OP_PREAD;
    cb.rw_flags  = 0;                 /* 一次性内存 */
    cb.obj.fd    = fd;
    cb.offset    = 0;
    cb.user_data = 0x1234;
    cb.iov       = &iov;
    cb.iov_cnt   = 1;
    cb.host_pid  = 0;                 /* 当前进程 */

    ret = nds_io_submit(ctx, 1, &cb);
    assert(ret == 1);                 /* 已被接受 */

    /* 6. 收割完成：成功返回 1 */
    ret = nds_io_getevents(ctx, 1, 1, &ev, NULL);
    assert(ret == 1);
    assert(ev.user_data == 0x1234);
    assert(ev.res == 0);              /* 0 = 成功；负值 = -errno */

    /* 7. 拆除 */
    nds_io_destroy_ctx(ctx);
    nds_unregister_fs(&fs);
    nds_exit();
    close(fd);
    aclrtFree(hbm);
    aclrtResetDevice(0);
    aclFinalize();
    return 0;
}
```

**注册内存（热路径）用法：**

```c
/* hbm 来自 aclrtMalloc */
ret = nds_register_mem(hbm, BUF_SIZE, 0);
assert(ret == 0);

/* 之后每次 I/O 使用已注册窗口 */
cb.rw_flags = NDS_IO_F_REGISTERED_MEM;
cb.host_pid = 0;                      /* 注册内存时必须为 0 */

nds_unregister_mem(hbm, BUF_SIZE, 0); /* size 必须与注册时完全一致 */
```

## 3.3 Python 使用示例

Python 扩展模块名为 `nds`：

```python
import os
import torch
import nds

# 初始化
assert nds.init() == 0

# 注册文件系统：传入 fd 序列
topo_fd = os.open("/mnt/nvme/data.bin", os.O_RDONLY)
assert nds.register_fs([topo_fd]) == 0

# 创建上下文：返回 PyCapsule，或负 errno
ctx = nds.io_new_ctx(64)
assert not isinstance(ctx, int) or ctx >= 0

file_fd = os.open("/mnt/nvme/data.bin", os.O_RDWR | os.O_DIRECT)

# 用 PyTorch 在 NPU 上分配 HBM
tensor = torch.empty(1 << 20, dtype=torch.uint8, device="npu:0")
hbm_addr = tensor.data_ptr()

# iocb: (opcode, fd, offset, iov[, rw_flags=0, host_pid=0, user_data=0])
# iov : [(buf_addr, buf_len), ...]
ret = nds.io_submit(ctx, [
    (nds.NDS_IO_OP_PREAD, file_fd, 0, [(hbm_addr, tensor.numel())],
     0, 0, 0x1234),
])
assert ret == 1

# 收割：返回 [(user_data, res), ...]，失败返回负 errno
events = nds.io_getevents(ctx, 1, 1, timeout_sec=5.0)
assert events == [(0x1234, 0)]

nds.io_destroy_ctx(ctx)
nds.unregister_fs([topo_fd])
nds.exit()
os.close(file_fd)
os.close(topo_fd)
```

---

# 修订历史

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| v1 | 2026-09-24 | 初版。基于 `file_p2p/nds_api.h`（`NDS_API_VERSION == 1`）与 libcufile 1.19.1.55 的 `cufile.h`，包含接口总览、接口对比与接口使用三部分。 |
