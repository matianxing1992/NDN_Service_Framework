import asyncio
import argparse
import time
import grpc
import helloworld_pb2
import helloworld_pb2_grpc

# 路径根据你的实际环境调整
CERT_PATH = '/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC'

async def send_request(stub, i, timeout_s, quiet=False, measured=True):
    start_time = time.perf_counter()
    response = await stub.SayHello(helloworld_pb2.HelloRequest(name=f"Test-{i}"),
                                   timeout=timeout_s)
    latency = (time.perf_counter() - start_time) * 1000
    if not quiet and measured:
        print(f"request={i} latency_ms={latency:.3f} message={response.message}", flush=True)
    return latency

async def run():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", default="10.0.0.58:50051")
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--interval-ms", type=float, default=0.0)
    parser.add_argument("--rate-rps", type=float, default=0.0)
    parser.add_argument("--duration-s", type=float, default=0.0)
    parser.add_argument("--timeout-s", type=float, default=20.0)
    parser.add_argument("--warmup-s", type=float, default=0.0)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--skip-channel-ready", action="store_true")
    args = parser.parse_args()

    # 读取证书
    # with open(f'{CERT_PATH}/ca.pem', 'rb') as f:
    #     trusted_certs = f.read()
    # with open(f'{CERT_PATH}/client.key', 'rb') as f:
    #     client_key = f.read()
    # with open(f'{CERT_PATH}/client.pem', 'rb') as f:
    #     client_cert = f.read()

    # 创建 SSL 凭据
    # credentials = grpc.ssl_channel_credentials(
    #     root_certificates=trusted_certs,
    #     private_key=client_key,
    #     certificate_chain=client_cert
    # )

    # 异步安全通道（如果需要使用安全连接则替换下行）
    channel_options = [
        ("grpc.max_concurrent_streams", 1024),
        ("grpc.keepalive_time_ms", 10000),
        ("grpc.keepalive_timeout_ms", 5000),
        ("grpc.http2.max_pings_without_data", 0),
    ]
    async with grpc.aio.insecure_channel(args.target, options=channel_options) as channel:
        if not args.skip_channel_ready:
            await channel.channel_ready()
        stub = helloworld_pb2_grpc.GreeterStub(channel)

        if args.rate_rps > 0 and args.warmup_s > 0:
            warmup_interval_s = 1.0 / args.rate_rps
            warmup_start = time.monotonic()
            next_send = warmup_start
            warmup_tasks = []
            i = 0
            while time.monotonic() - warmup_start < args.warmup_s:
                now = time.monotonic()
                if now < next_send:
                    await asyncio.sleep(next_send - now)
                warmup_tasks.append(asyncio.create_task(
                    send_request(stub, -i - 1, args.timeout_s, quiet=True, measured=False)))
                i += 1
                next_send += warmup_interval_s
            await asyncio.gather(*warmup_tasks, return_exceptions=True)

        latencies = []
        if args.rate_rps > 0 and args.duration_s > 0:
            interval_s = 1.0 / args.rate_rps
            start = time.monotonic()
            next_send = start
            tasks = []
            i = 0
            while time.monotonic() - start < args.duration_s:
                now = time.monotonic()
                if now < next_send:
                    await asyncio.sleep(next_send - now)
                tasks.append(asyncio.create_task(
                    send_request(stub, i, args.timeout_s, quiet=args.quiet)))
                i += 1
                next_send += interval_s
            results = await asyncio.gather(*tasks, return_exceptions=True)
            failures = 0
            for result in results:
                if isinstance(result, Exception):
                    failures += 1
                    print(f"request_failed error={result}", flush=True)
                else:
                    latencies.append(result)
            print(f"GRPC_CLIENT_RATE sent={len(tasks)} success={len(latencies)} "
                  f"failures={failures} duration_s={args.duration_s:.3f} "
                  f"offered_rps={args.rate_rps:.3f} "
                  f"actual_success_rps={len(latencies) / args.duration_s:.3f}",
                  flush=True)
        else:
            for i in range(args.count):
                latencies.append(await send_request(stub, i, args.timeout_s, quiet=args.quiet))
                if args.interval_ms > 0 and i + 1 < args.count:
                    await asyncio.sleep(args.interval_ms / 1000.0)

        if not latencies:
            print("GRPC_CLIENT_SUMMARY count=0 avg_ms=0 p50_ms=0 p95_ms=0 min_ms=0 max_ms=0",
                  flush=True)
            return
        sorted_latencies = sorted(latencies)
        avg_latency = sum(latencies) / len(latencies)
        p50 = sorted_latencies[len(sorted_latencies) // 2]
        p95 = sorted_latencies[int(0.95 * (len(sorted_latencies) - 1))]
        print(f"GRPC_CLIENT_SUMMARY count={len(latencies)} avg_ms={avg_latency:.3f} "
              f"p50_ms={p50:.3f} p95_ms={p95:.3f} "
              f"min_ms={sorted_latencies[0]:.3f} max_ms={sorted_latencies[-1]:.3f}",
              flush=True)

if __name__ == "__main__":
    asyncio.run(run())
