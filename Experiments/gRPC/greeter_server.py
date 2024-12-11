import grpc
from concurrent import futures
import time
import helloworld_pb2
import helloworld_pb2_grpc

class Greeter(helloworld_pb2_grpc.GreeterServicer):
    def SayHello(self, request, context):
        print("Received request: " + request.name)
        return helloworld_pb2.HelloReply(message='Hello, {}'.format(request.name))

def serve():
    # 读取证书文件
    with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/server.key', 'rb') as f:
        server_key = f.read()
    with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/server.pem', 'rb') as f:
        server_cert = f.read()
    with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/ca.pem', 'rb') as f:
        trusted_certs = f.read()

    # 创建 SSL 证书对象
    server_credentials = grpc.ssl_server_credentials(
        ((server_key, server_cert),),
        root_certificates=trusted_certs,
        require_client_auth=True
    )

    # 创建 gRPC 服务器
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    helloworld_pb2_grpc.add_GreeterServicer_to_server(Greeter(), server)
    
    # 绑定服务器到指定端口并启用安全通道
    server.add_secure_port('10.0.0.1:50051', server_credentials)
    server.start()
    print("Server started, listening on port 50051")

    try:
        while True:
            time.sleep(86400)  # 让服务器持续运行
    except KeyboardInterrupt:
        server.stop(0)

if __name__ == '__main__':
    serve()

