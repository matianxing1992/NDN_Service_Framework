# MiniNDN Owned Network Observation

2026-09-08，恢复此前被用户优先NDN小例子打断的已实现工作并纳入应用源码seal。
`Experiments/minindn_network_resources.py`只观测本次MiniNDN的PID/startTicks、
net namespace与root接口；创建/启动两份清单取并集，stop后有限扫描实际线程、
namespace FD、nsfs mount和接口。不可读取/超时/资源仍在均不得标clean。
它不删除其他namespace，不用进程退出单独推断网络销毁。

既有证据保留于`results/spec183-network-resources-20260908`：首组12项中11通过，
1项fixture相对symlink错误；修正后6项fixture通过（与前组重叠）。真实root
kernel-ordered.xml为1项PASS：unshare net namespace在子进程退出后仍由FD保留，
关闭FD后消失。不是MiniNDN/YOLO运行。没有重跑这组未变kernel实验。

初次root解释器无pytest，后一次site-packages插入过早遮蔽stdlib argparse，
均为测试启动失败；最终只把用户包插入stdlib之后，禁用无关pytest插件。
当前新增sealer文件清单项，实际User的外置目录help通过；相关source sealer
6项通过，结果在`yolo-layered-20260908/source-helper-closure.xml`。
三类正式MiniNDN场景、T007与T010完整验收仍未关闭。
