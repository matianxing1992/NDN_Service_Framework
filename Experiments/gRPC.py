from mininet.net import Mininet
from mininet.node import Controller
from mininet.topo import Topo
from mininet.cli import CLI
from mininet.log import setLogLevel, info

class MyTopo(Topo):
    def build(self):
        # 创建主机
        h1 = self.addHost('h1')
        h2 = self.addHost('h2')

        # 创建交换机
        s1 = self.addSwitch('s1')

        # 将主机连接到交换机，并设置链路参数
        self.addLink(h1, s1, delay='10ms')
        self.addLink(h2, s1, delay='10ms')

def run():
    # 设置日志级别
    setLogLevel('info')
    
    # 创建自定义拓扑
    topo = MyTopo()

    # 创建 Mininet 网络
    net = Mininet(topo=topo, controller=Controller)

    # 启动网络
    net.start()

    # 测试网络连通性
    info("Testing network connectivity\n")
    net.pingAll()

    # 启动 CLI
    info("Starting CLI\n")
    
    # h1 tc qdisc add dev h1-eth0 root netem delay 10ms loss 5
    # h2 tc qdisc add dev h2-eth0 root netem delay 10ms loss 5
    # h1 xterm -T "h1" &
    # h2 xterm -T "h2" &
    # source /home/tianxing/NDN/ndn-service-framework/Experiments/pythonEnv/bin/activate
    # python /home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/greeter_server.py
    # python /home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/greeter_client.py
    
    CLI(net)

    # 停止网络
    net.stop()

if __name__ == '__main__':
    run()

