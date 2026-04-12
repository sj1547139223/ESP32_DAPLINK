import asyncio, websockets, json, time

async def test_latency():
    ws = await websockets.connect('ws://47.103.38.218:7000', open_timeout=30)
    await ws.send(json.dumps({'type':'register','role':'client','code':'TEST99'}))
    
    # No peer connected, so binary messages won't get a response.
    # Just measure send latency.
    times = []
    for i in range(10):
        t1 = time.perf_counter()
        await ws.send(b'\x00\x00')
        t2 = time.perf_counter()
        times.append((t2-t1)*1000)
    
    await ws.close()
    print(f'WS send times (ms): {[f"{t:.1f}" for t in times]}')
    print(f'Average send: {sum(times)/len(times):.1f}ms')

asyncio.run(test_latency())
