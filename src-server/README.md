# src-server —— 仅服务端代码

这个目录里的 `**.cpp` 只在 `--target_type=server` 构建时参与编译（见 `xmake.lua`）。

本项目是客户端模组框架，平时用不到；只有当你需要做「客户端 + 服务端」双端模组、
把服务端专有的实现（如 `ll/api/event/server/*`、BDS 专有 API）隔离出来时才用到。
