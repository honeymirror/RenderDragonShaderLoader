#pragma once

namespace renderdragon_shader_loader {

// 让游戏窗口接受 Windows 文件拖放（WM_DROPFILES）：*.zip 或已解压目录会复制到
// data/shaderpacks/，然后由客户端线程重扫并刷新面板。
void installDragDropHook();
void uninstallDragDropHook();

} // namespace renderdragon_shader_loader
