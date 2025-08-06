import grpc
import asyncio
from datetime import datetime
import helloworld_pb2
import helloworld_pb2_grpc

# 路径根据你的实际环境调整
CERT_PATH = '/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC'

async def send_request(stub, i):
    start_time = datetime.now()
    response = await stub.SayHello(helloworld_pb2.HelloRequest(name=f"Test-{i}"))
    end_time = datetime.now()
    latency = (end_time - start_time).total_seconds() * 1000
    return latency

async def run():
    # 读取证书
    with open(f'{CERT_PATH}/ca.pem', 'rb') as f:
        trusted_certs = f.read()
    with open(f'{CERT_PATH}/client.key', 'rb') as f:
        client_key = f.read()
    with open(f'{CERT_PATH}/client.pem', 'rb') as f:
        client_cert = f.read()

    # 创建 SSL 凭据
    credentials = grpc.ssl_channel_credentials(
        root_certificates=trusted_certs,
        private_key=client_key,
        certificate_chain=client_cert
    )

    # 异步安全通道（如果需要使用安全连接则替换下行）
    async with grpc.aio.insecure_channel('10.0.0.58:50051') as channel:
        stub = helloworld_pb2_grpc.GreeterStub(channel)

        latencies = []
        tasks = []
        for i in range(1000):
            task = asyncio.create_task(send_request(stub, i))
            tasks.append(task)
            await asyncio.sleep(0.0066)  # 控制发包频率，例如 5ms 一个请求

        # 等待所有请求完成
        results = await asyncio.gather(*tasks)
        avg_latency = sum(results) / len(results)
        print(f"Average latency over 1000 requests: {avg_latency:.2f} ms")

if __name__ == "__main__":
    asyncio.run(run())
