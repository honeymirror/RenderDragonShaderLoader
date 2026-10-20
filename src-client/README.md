# src-client —— 仅客户端代码

这个目录里的 `**.cpp` 只在 `--target_type=client` 构建时参与编译（见 `xmake.lua`），
并会被加入 include 路径，因此可以：

```cpp
#include "MyClientOnlyHeader.h"   // 直接引用本目录的头文件
```

适合放：按键绑定、UI/渲染、客户端指令、客户端事件处理等只在客户端存在的东西。
对应的客户端专有头文件（`ll/api/input/*`、`ll/api/event/input/*`、`ll/api/event/render/*`、
`ll/api/event/client/*` 等）也只在客户端构建时可用，写在这里最安全。

新建 `.cpp` 后无需改 `xmake.lua`，`src-client/**.cpp` 会被自动收集。
