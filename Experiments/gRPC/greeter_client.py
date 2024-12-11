import grpc
from datetime import datetime
import helloworld_pb2
import helloworld_pb2_grpc
import time

def get_formatted_time():
    current_time = datetime.now()
    formatted_time = current_time.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    return formatted_time



def run():
    # 读取证书文件
    with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/ca.pem', 'rb') as f:
        trusted_certs = f.read()
    with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/client.key', 'rb') as f:
        client_key = f.read()
    with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/client.pem', 'rb') as f:
        client_cert = f.read()

    # 创建 SSL 证书对象
    credentials = grpc.ssl_channel_credentials(root_certificates=trusted_certs,
                                               private_key=client_key,
                                               certificate_chain=client_cert)

    # 使用安全通道连接服务器
    with grpc.secure_channel('10.0.0.1:50051', credentials) as channel:
        # calculate the average time for 100 requests
        total = 0.0
        for i in range(100):
            stub = helloworld_pb2_grpc.GreeterStub(channel)

            # 获取当前时间
            start_time = datetime.now()

            response = stub.SayHello(helloworld_pb2.HelloRequest(name="Test"))

            # 获取当前时间
            end_time = datetime.now()
            # 计算请求时间
            total = total + (end_time-start_time).total_seconds() * 1000
            time.sleep(0.015)
            
        print("Average time for 100 requests: " + str(total / 100) + " ms")

if __name__ == "__main__":
    run()

