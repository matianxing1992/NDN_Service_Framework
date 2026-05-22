import grpc
from concurrent import futures
import argparse
import time
import helloworld_pb2
import helloworld_pb2_grpc

class Greeter(helloworld_pb2_grpc.GreeterServicer):
    def __init__(self, delay_ms=0, quiet=False):
        self.delay_s = max(0, delay_ms) / 1000.0
        self.quiet = quiet

    def SayHello(self, request, context):
        if not self.quiet:
            print("Received request: " + request.name)
        if self.delay_s > 0:
            time.sleep(self.delay_s)
        return helloworld_pb2.HelloReply(message='Hello, {}'.format(request.name))

def serve():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0:50051")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--workers", type=int, default=32)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    # 读取证书文件
    # with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/server.key', 'rb') as f:
    #     server_key = f.read()
    # with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/server.pem', 'rb') as f:
    #     server_cert = f.read()
    # with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/ca.pem', 'rb') as f:
    #     trusted_certs = f.read()

    # 创建 SSL 证书对象
    # server_credentials = grpc.ssl_server_credentials(
    #     ((server_key, server_cert),),
    #     root_certificates=trusted_certs,
    #     require_client_auth=True
    # )

    # 创建 gRPC 服务器
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=args.workers))
    helloworld_pb2_grpc.add_GreeterServicer_to_server(Greeter(args.delay_ms, args.quiet), server)
    
    # 绑定服务器到指定端口并启用安全通道
    # server.add_secure_port('10.0.0.58:50051', server_credentials)
    server.add_insecure_port(args.bind)
    
    server.start()
    print(f"GRPC_SERVER_READY bind={args.bind} delay_ms={args.delay_ms}", flush=True)

    try:
        while True:
            time.sleep(86400)  # 让服务器持续运行
    except KeyboardInterrupt:
        server.stop(0)

if __name__ == '__main__':
    serve()
