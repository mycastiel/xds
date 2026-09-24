# NDS 约束文档

## 关闭IOMMU

NDS直通需要关闭IOMMU。`passthrough`模式也不行，性能差。

## 支持的块设备类型

不支持启用了NVMe多路径的nvme设备，通过以下方式确认：

```
# test -d /sys/block/nvmeXnY/mq || echo "mult-path device is not supported"
multi-path device is not supported
```

输出`multi-path device is not supported`，说明启用了NVMe多路径，

依赖`/dev/nvmeXnY`支持SGL，通过以下方式确认：

```
# nvme id-ctrl /dev/nvmeX -H | grep -i scatter
  [1:0] : 0x2 Scatter-Gather Lists Supported. Dwoard alignment required
```

有`Scatter-Gather Lists Supported`输出，说明支持`SGL`

只支持逻辑块大小为512B的NVMe设备或者对应的NVMe组成的linear LVM和RAID0设备。

| 配置                                    | 备注                                    |
| ------------------------------------- | ------------------------------------- |
| nvmeXnY或者nvmeXnYpZ                    | nvme块设备或者块设备的分区                       |
| 多个nvmeXnY或者nvmeXnYpZ组成的RAID0          | 通过mdadm创建，并且nvmeXnY或者nvmeXnYpZ的大小必须相同 |
| 多个nvmeXnY或者nvmeXnYpZ组成的线性(linear)LVM卷 | 通过lvm或者dmsetup创建                      |

## 支持的文件系统类型

裸块设备或者文件系统（ext4、XFS）

## 直通IO的对齐

文件 `offset`、HBM 缓冲区地址、长度均必须 **512 字节**（XDS 的扇区粒度）对齐，否则 I/O 返回 `-EINVAL`，无 fallback。

## 容器部署

### 插入内核模块

需要在Host上运行insmod: `insmod ./p2p_dev.ko`

### 依赖的文件路径

NDS依赖以下文件路径：

| 配置                      | 备注                             |
| ------------------------- | -------------------------------- |
| /sys/dev/block/           | 只读。从sysfs获取块设备的信息    |
| /dev/mapper/control       | 可读写。获取线性LVM卷的信息      |
| /dev/p2p\_device          | 可读写。XDS字符设备              |

