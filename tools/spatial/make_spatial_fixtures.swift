// SPDX-License-Identifier: Apache-2.0
//
// make_spatial_fixtures.swift (#154) — write tiny MV-HEVC videos and stereo HEIC photos the way
// a phone's spatial capture does, using the platform encoders (VideoToolbox MV-HEVC through
// AVAssetWriter's tagged-buffer adaptor; ImageIO's stereo-pair image group for HEIC). macOS 15+.
//
//   swift tools/spatial/make_spatial_fixtures.swift <out-dir> [width height frames]
//
// Each eye is a recognisable picture so a test can tell them apart AND tell their order:
//   - the LEFT eye has a RED band across the top, the RIGHT eye a BLUE band;
//   - a white square sits 6 % of the width further RIGHT in the left eye than in the right
//     eye (crossed disparity: the square floats in front of the screen);
//   - the rest is a static noise plate shifted by the same amount, so a disparity estimator
//     has texture to lock onto.
// Outputs:
//   mvhevc_lr.mov       base layer = left eye (the common case)
//   mvhevc_rl.mov       base layer = RIGHT eye (eye order must come from the metadata)
//   stereo_lr.heic      HEIF stereo pair group, image 0 = left
//   stereo_rl.heic      HEIF stereo pair group, image 0 = RIGHT
//   mono.heic           an ordinary HEIC (left-eye picture, no group)
// The fixtures under tests/fixtures/ were made with the default size (128x96, 3 frames).

import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import ImageIO
import UniformTypeIdentifiers
import VideoToolbox

let args = CommandLine.arguments
guard args.count >= 2 else {
    FileHandle.standardError.write("usage: make_spatial_fixtures.swift <out-dir> [w h frames]\n".data(using: .utf8)!)
    exit(2)
}
let outDir = URL(fileURLWithPath: args[1], isDirectory: true)
let W = args.count > 3 ? Int(args[2])! : 128
let H = args.count > 3 ? Int(args[3])! : 96
let FRAMES = args.count > 4 ? Int(args[4])! : 3
try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

// Deterministic noise plate, wider than an eye so both eyes crop from it.
var rng: UInt64 = 0x9E37_79B9_7F4A_7C15
func nextByte() -> UInt8 {
    rng ^= rng << 13
    rng ^= rng >> 7
    rng ^= rng << 17
    return UInt8(truncatingIfNeeded: rng >> 24)
}
let plateW = W * 2
var plate = [UInt8](repeating: 0, count: plateW * H)
for i in 0..<plate.count { plate[i] = 40 + nextByte() / 3 }

let disparity = max(2, W * 6 / 100)

// RGBA8 pixels for one eye. `left` picks the band colour and the crop offset.
func eyePixels(left: Bool, frame: Int) -> [UInt8] {
    var px = [UInt8](repeating: 255, count: W * H * 4)
    let off = left ? 0 : disparity  // the right eye sees the plate shifted
    let band = max(4, H / 8)
    let sq = W / 4
    let sqX = W / 2 - sq / 2 + (left ? disparity / 2 : -disparity / 2) + frame
    let sqY = H / 2 - sq / 2
    for y in 0..<H {
        for x in 0..<W {
            let o = (y * W + x) * 4
            var r = plate[y * plateW + x + off]
            var g = r
            var b = r
            if y < band {
                if left { r = 230; g = 20; b = 20 } else { r = 20; g = 20; b = 230 }
            } else if x >= sqX && x < sqX + sq && y >= sqY && y < sqY + sq {
                r = 250; g = 250; b = 250
            }
            px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = 255
        }
    }
    return px
}

func pixelBuffer(_ rgba: [UInt8]) -> CVPixelBuffer {
    var pb: CVPixelBuffer?
    let attrs: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
    CVPixelBufferCreate(nil, W, H, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb)
    let b = pb!
    CVPixelBufferLockBaseAddress(b, [])
    let base = CVPixelBufferGetBaseAddress(b)!.assumingMemoryBound(to: UInt8.self)
    let stride = CVPixelBufferGetBytesPerRow(b)
    for y in 0..<H {
        for x in 0..<W {
            let s = (y * W + x) * 4
            let d = y * stride + x * 4
            base[d] = rgba[s + 2]; base[d + 1] = rgba[s + 1]; base[d + 2] = rgba[s]; base[d + 3] = 255
        }
    }
    CVPixelBufferUnlockBaseAddress(b, [])
    return b
}

func cgImage(_ rgba: [UInt8]) -> CGImage {
    let data = CFDataCreate(nil, rgba, rgba.count)!
    return CGImage(width: W, height: H, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: W * 4,
                   space: CGColorSpace(name: CGColorSpace.sRGB)!,
                   bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                   provider: CGDataProvider(data: data)!, decode: nil, shouldInterpolate: false,
                   intent: .defaultIntent)!
}

// ---- MV-HEVC ---------------------------------------------------------------------------
// baseIsLeft=false puts the RIGHT eye in layer 0 / view 0: LeftAndRightViewIDs = [1, 0].
func writeMvHevc(_ name: String, baseIsLeft: Bool) throws {
    let url = outDir.appendingPathComponent(name)
    try? FileManager.default.removeItem(at: url)
    let writer = try AVAssetWriter(outputURL: url, fileType: .mov)
    let leftView = baseIsLeft ? 0 : 1
    let rightView = baseIsLeft ? 1 : 0
    let compression: [CFString: Any] = [
        kVTCompressionPropertyKey_MVHEVCVideoLayerIDs: [0, 1],
        kVTCompressionPropertyKey_MVHEVCViewIDs: [0, 1],
        kVTCompressionPropertyKey_MVHEVCLeftAndRightViewIDs: [leftView, rightView],
        kVTCompressionPropertyKey_HasLeftStereoEyeView: true,
        kVTCompressionPropertyKey_HasRightStereoEyeView: true,
    ]
    let settings: [String: Any] = [
        AVVideoCodecKey: AVVideoCodecType.hevc,
        AVVideoWidthKey: W, AVVideoHeightKey: H,
        AVVideoCompressionPropertiesKey: compression,
    ]
    let input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
    input.expectsMediaDataInRealTime = false
    let adaptor = AVAssetWriterInputTaggedPixelBufferGroupAdaptor(assetWriterInput: input,
                                                                 sourcePixelBufferAttributes: nil)
    writer.add(input)
    guard writer.startWriting() else { throw writer.error ?? NSError(domain: "mvhevc", code: 1) }
    writer.startSession(atSourceTime: .zero)
    for f in 0..<FRAMES {
        while !input.isReadyForMoreMediaData { Thread.sleep(forTimeInterval: 0.01) }
        let l = CMTaggedBuffer(tags: [.stereoView(.leftEye), .videoLayerID(Int64(leftView))],
                               pixelBuffer: pixelBuffer(eyePixels(left: true, frame: f)))
        let r = CMTaggedBuffer(tags: [.stereoView(.rightEye), .videoLayerID(Int64(rightView))],
                               pixelBuffer: pixelBuffer(eyePixels(left: false, frame: f)))
        let t = CMTime(value: CMTimeValue(f), timescale: 30)
        guard adaptor.appendTaggedBuffers([l, r], withPresentationTime: t) else {
            throw writer.error ?? NSError(domain: "mvhevc", code: 2)
        }
    }
    input.markAsFinished()
    let done = DispatchSemaphore(value: 0)
    writer.finishWriting { done.signal() }
    done.wait()
    if writer.status != .completed { throw writer.error ?? NSError(domain: "mvhevc", code: 3) }
    print("wrote \(url.path)")
}

// ---- stereo HEIC -----------------------------------------------------------------------
func writeStereoHeic(_ name: String, firstIsLeft: Bool) throws {
    let url = outDir.appendingPathComponent(name)
    try? FileManager.default.removeItem(at: url)
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.heic.identifier as CFString, 2, nil)
    else { throw NSError(domain: "heic", code: 1) }
    func props(left: Bool) -> CFDictionary {
        var group: [CFString: Any] = [
            kCGImagePropertyGroupIndex: 0,
            kCGImagePropertyGroupType: kCGImagePropertyGroupTypeStereoPair,
        ]
        group[left ? kCGImagePropertyGroupImageIsLeftImage : kCGImagePropertyGroupImageIsRightImage] = true
        group[kCGImagePropertyGroupImageDisparityAdjustment] = 0
        return [kCGImagePropertyGroups: group, kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary
    }
    for left in (firstIsLeft ? [true, false] : [false, true]) {
        CGImageDestinationAddImage(dest, cgImage(eyePixels(left: left, frame: 0)), props(left: left))
    }
    guard CGImageDestinationFinalize(dest) else { throw NSError(domain: "heic", code: 2) }
    print("wrote \(url.path)")
}

// An ordinary (flat) HEIC: the left-eye picture alone, no group.
func writeMonoHeic(_ name: String) throws {
    let url = outDir.appendingPathComponent(name)
    try? FileManager.default.removeItem(at: url)
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.heic.identifier as CFString, 1, nil)
    else { throw NSError(domain: "heic", code: 3) }
    CGImageDestinationAddImage(dest, cgImage(eyePixels(left: true, frame: 0)),
                               [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary)
    guard CGImageDestinationFinalize(dest) else { throw NSError(domain: "heic", code: 4) }
    print("wrote \(url.path)")
}

do {
    try writeMvHevc("mvhevc_lr.mov", baseIsLeft: true)
    try writeMvHevc("mvhevc_rl.mov", baseIsLeft: false)
    try writeStereoHeic("stereo_lr.heic", firstIsLeft: true)
    try writeStereoHeic("stereo_rl.heic", firstIsLeft: false)
    try writeMonoHeic("mono.heic")
} catch {
    FileHandle.standardError.write("error: \(error)\n".data(using: .utf8)!)
    exit(1)
}
