# Dependency TODO

- 内网 RPM 打包系统可能不方便执行 `git clone`。
- 内网 RPM 打包系统可能不方便进行 Golang 联网依赖下载或 `go install`。
- 后续需要把外部依赖获取与正常构建流程解耦，避免在 `build.sh` 和 CMake 构建过程中直接访问远端。
- C/C++ 侧外部依赖需要支持本地源码包、内部镜像或预置目录，而不是只支持在线拉取。
- Golang 侧需要评估 `vendor`、内部 `goproxy` 或预置工具链方案，避免在 RPM 打包阶段联网。
