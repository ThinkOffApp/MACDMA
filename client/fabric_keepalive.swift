// SPDX-License-Identifier: Apache-2.0
// MCDMA Metal keepalive. See docs/gpu-keepalive.md for measurements and limits.
// This optional userspace process submits GPU work; it does not configure RDMA.
import Foundation
import Metal

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data(("fabric-keepalive: \(message)\n").utf8))
    exit(2)
}

let usage = "Usage: fabric-keepalive [seconds|0=until-stopped] [small|medium]\nStop with Ctrl-C. This keeps the GPU busy and consumes power."
let args = Array(CommandLine.arguments.dropFirst())
if args == ["--help"] || args == ["-h"] {
    print(usage)
    exit(0)
}
guard args.count <= 2,
      let seconds = Double(args.first ?? "0"), seconds.isFinite, seconds >= 0 else {
    fail(usage)
}
let mode = args.count > 1 ? args[1] : "small"
guard mode == "small" || mode == "medium" else { fail(usage) }
guard let device = MTLCreateSystemDefaultDevice(),
      let queue = device.makeCommandQueue(maxCommandBufferCount: 4) else {
    fail("no Metal device or command queue")
}

// Preserve the measured small workload: 16,384 threads, 64 iterations,
// one dispatch at a time, with no deliberate sleep between dispatches.
let source = """
#include <metal_stdlib>
using namespace metal;
kernel void keepalive(device float *sink [[buffer(0)]], constant uint &iterations [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    float v = sink[id];
    for (uint i = 0; i < iterations; ++i) v = v * 1.000001f + 0.25f;
    sink[id] = v;
}
"""
let pipeline: MTLComputePipelineState
do {
    let library = try device.makeLibrary(source: source, options: nil)
    guard let function = library.makeFunction(name: "keepalive") else {
        fail("Metal kernel unavailable")
    }
    pipeline = try device.makeComputePipelineState(function: function)
} catch {
    fail("Metal compilation failed: \(error)")
}
let threads = mode == "medium" ? 1 << 18 : 1 << 14
guard let sink = device.makeBuffer(length: threads * MemoryLayout<Float>.stride,
                                   options: .storageModeShared) else {
    fail("Metal buffer allocation failed")
}
memset(sink.contents(), 0, sink.length)

func dispatch() -> MTLCommandBuffer {
    guard let command = queue.makeCommandBuffer(),
          let encoder = command.makeComputeCommandEncoder() else {
        fail("Metal command allocation failed")
    }
    var iterations: UInt32 = 64
    encoder.setComputePipelineState(pipeline)
    encoder.setBuffer(sink, offset: 0, index: 0)
    encoder.setBytes(&iterations, length: MemoryLayout<UInt32>.size, index: 1)
    let width = min(threads, pipeline.maxTotalThreadsPerThreadgroup)
    encoder.dispatchThreads(MTLSize(width: threads, height: 1, depth: 1),
                            threadsPerThreadgroup: MTLSize(width: width, height: 1, depth: 1))
    encoder.endEncoding()
    command.commit()
    return command
}

func wait(_ command: MTLCommandBuffer) {
    command.waitUntilCompleted()
    guard command.status == .completed else {
        fail("Metal execution failed: \(String(describing: command.error))")
    }
}

print("fabric-keepalive mode=\(mode) seconds=\(seconds) threads=\(threads); stop with Ctrl-C")
fflush(stdout)
let started = ProcessInfo.processInfo.systemUptime
var count = 1
var inflight = dispatch()
while seconds == 0 || ProcessInfo.processInfo.systemUptime - started < seconds {
    wait(inflight)
    inflight = dispatch()
    count += 1
    if count % 5000 == 0 {
        print("fabric-keepalive mode=\(mode) dispatches=\(count)")
        fflush(stdout)
    }
}
wait(inflight)
print("fabric-keepalive done mode=\(mode) dispatches=\(count) threads=\(threads)")
