SMCAMDProcessor
========
[![Github release](https://img.shields.io/github/downloads/htmambo/SMCAMDProcessor/total.svg?color=pink)](https://github.com/htmambo/SMCAMDProcessor/releases)
![Github release](https://img.shields.io/github/repo-size/htmambo/SMCAMDProcessor.svg?color=blue)
[![Continuous Delivery](https://github.com/htmambo/SMCAMDProcessor/actions/workflows/main.yml/badge.svg)](https://github.com/htmambo/SMCAMDProcessor/actions/workflows/main.yml)

原版[README.md](https://github.com/htmambo/SMCAMDProcessor/blob/master/README_EN.md)  

# 说明
这是我基于[trulyspinach/SMCAMDProcessor](https://github.com/trulyspinach/SMCAMDProcessor)的一些修改，所有代码均来自于网络搬运，目前实现了：  

1. 修复0.7.x后在`iStat Menus`中显示CPU温度；  
2. 添加CPU功耗、GPU温度的SMCKey输出；  
3. AMDRyzenCPUPowerManagement.kext中支持设置开机时的CPB状态以及频率定义；
4. 睡眠唤醒后恢复睡睡眠前的CPB状态及频率设置。 

## AMDRyzenCPUPowerManagement.kext 新加的属性：

属性|默认|备注
---|---|---
CPBStatus|0|CPB状态（0：关闭，1：启用）
SpeedID|1|频率表中的ID值，具体代表的频率请自行打开`AMD Power Gadget.app`后在选项的`Speed`->`Advanced Options`中查询

## 本地构建

```bash
./Scripts/build.sh
```

脚本会自动准备依赖（`MacKernelSDK` 子模块、`Lilu.kext`、`VirtualSMC`）、构建全部 4 个 target，
并输出 `build/Release.zip`（解压后根目录为 `AMDRyzenCPUPowerManagement.kext`、
`SMCAMDProcessor.kext`、`AMD Power Gadget.app`）与产物版本摘要。

常用参数：`--clean`（全量重建）、`--configuration Debug`、`--skip-deps`、`--no-zip`、`--help`。

> kext 版本号由工程文件中的 `AMDRYZEN_KEXT_VERSION` 统一维护，升版时改这一处即可。

# 截图

![istatmenus](imgs/istatmenus.png)

![sensei_home](imgs/sensei_home.png)

![sensei_sensors](imgs/sensei_sensors.png)


## 注意事项
* 我对macOS内核开发`一窍不通`，针对这个软件项目的修改也仅仅只是因为`喜欢折腾`而已。

* 本人不承担您因为使用此软件所造成的一切损失！

[![stats](https://github-readme-stats.vercel.app/api?username=htmambo&theme=radical)](https://github.com/anuraghazra/github-readme-stats)
