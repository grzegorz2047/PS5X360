// SPDX-License-Identifier: MIT
// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2, fix 44): the original file
// lived in the author's unpublished build/kinect-relay-20261015/emu-include. The package's
// virtual_depth_camera.hpp (KINECT-source-and-licenses.zip) includes it and uses
// kinect_usb::Sensor: a default constructor that starts it, a destructor that stops it,
// active(), CopyDepth() and CopyColor().
//
// Written from the official 1.0.2 executable (eboot.elf, va 0x746510-0x7481e4: constructor,
// sensor thread, relay client, camera search, stream session, control command; destructor
// 0x74be00; CopyDepth/CopyColor inlined at 0x748f67) and from the package's own
// native/kinect/kinect_tracker.c, which captures the camera the same way (libfreenect's
// control commands and registers, FreeBSD ugen isochronous transfers) and serves the relay.
// The log lines are the official ones, at the official levels.
//
// The sensor thread tries, in this order and again every 2 s while the emulated camera is
// open:
//   1. the relay: kinect-tracker.elf (started by the Helper payload) on 127.0.0.1:9301, one
//      client, frames as "KNCT", u8 stream (1 depth, else colour), 3 bytes, u32 LE size,
//      payload of 422400 (11-bit 640x480) or 307200 (Bayer 640x480) bytes;
//   2. a Kinect for Xbox 360 on USB (VID 045e, PID 02ae or 02bf) opened directly through
//      /dev/ugen*: depth on endpoint 0x82 and colour on 0x81, two 32-packet transfers each;
//   3. the relay again, then nothing: the camera shows the pose-derived frames
//      (virtual_depth_camera.hpp) until a source appears.
// While a source delivers frames, active() is true and the camera gives its frames and the
// factory calibration (/data/kinect-tracker/calibration.bin, factory_calibration.hpp).
//
// Not the original code: the operating-system calls of the direct path go through a policy
// class (SystemUsb), so the host check drives the same session with a simulated camera; and
// a packet longer than its 1920-byte slot is skipped (the official copies it unchecked).
#pragma once
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// The core's XELOGW/XELOGI. A host check defines XBOX360PS5_KINECT_USB_HOST_LOG and its own.
#ifndef XBOX360PS5_KINECT_USB_HOST_LOG
#include "xenia/base/logging.h"
#endif

namespace xbox360ps5::kinect_usb {

constexpr uint16_t kRelayPort = 9301;
constexpr uint32_t kDepthFrameBytes = 640 * 480 * 11 / 8;  // 0x67200
constexpr uint32_t kColorFrameBytes = 640 * 480;           // 0x4B000
constexpr uint16_t kVendor = 0x045E, kProduct1414 = 0x02AE, kProduct1473 = 0x02BF;
constexpr uint32_t kPacketBytes = 1920, kPackets = 32, kTransferBytes = kPacketBytes * kPackets;
constexpr uint8_t kEndpoints = 4;  // 0, 1: depth (0x82); 2, 3: colour (0x81)

inline bool IsKinectCamera(uint16_t vendor, uint16_t product) {
  return vendor == kVendor && (product == kProduct1473 || product == kProduct1414);
}

// FreeBSD dev/usb/usb_ioctl.h, as the official executable uses it (its ioctl numbers encode
// these sizes).
namespace ugen {
struct DeviceRequest {
  uint8_t bmRequestType, bRequest, wValue[2], wIndex[2], wLength[2];
};
struct CtlRequest {
  void* ucr_data;
  uint16_t ucr_flags, ucr_actlen;
  uint8_t ucr_addr;
  DeviceRequest ucr_request;
};
struct DeviceDescriptor {
  uint8_t bLength, bDescriptorType, bcdUSB[2], bDeviceClass, bDeviceSubClass, bDeviceProtocol,
      bMaxPacketSize, idVendor[2], idProduct[2], bcdDevice[2], iManufacturer, iProduct,
      iSerialNumber, bNumConfigurations;
};
struct FsEndpoint {
  void** ppBuffer;
  uint32_t* pLength;
  uint32_t nFrames, aFrames;
  uint16_t flags, timeout, isoc_time_complete;
  int status;
};
struct FsInit {
  FsEndpoint* pEndpoints;
  uint8_t ep_index_max;
};
struct FsOpen {
  uint32_t max_bufsize, max_frames;
  uint16_t max_packet_length;
  uint8_t dev_index, ep_index, ep_no;
};
static_assert(sizeof(CtlRequest) == 0x18 && sizeof(DeviceDescriptor) == 0x12 &&
              sizeof(FsEndpoint) == 0x28 && sizeof(FsInit) == 0x10 && sizeof(FsOpen) == 0x10);
constexpr unsigned long kGetDeviceDesc = 0x40125569, kDoRequest = 0xC018556F,
                        kFsStart = 0x800155C0, kFsStop = 0x800155C1, kFsComplete = 0x400155C2,
                        kFsInit = 0x801055C3, kFsUninit = 0x800155C4, kFsOpen = 0xC01055C5,
                        kFsClose = 0x800155C6;
constexpr uint16_t kShortTransferOk = 0x0004;            // USB_SHORT_XFER_OK
constexpr uint16_t kFsShortOk = 0x0003, kFsTimeout = 1000;  // SINGLE|MULTI_SHORT_OK, ms
}  // namespace ugen

// The camera's control command (libfreenect send_cmd 0x03, write_register): "GM", 2 words,
// command 3, tag, register, value, all little-endian.
inline void WriteRegisterCommand(uint16_t tag, uint16_t reg, uint16_t value, uint8_t out[12]) {
  const uint16_t words[6] = {0x4D47, 2, 3, tag, reg, value};
  for (int n = 0; n < 6; ++n) {
    out[n * 2] = uint8_t(words[n]);
    out[n * 2 + 1] = uint8_t(words[n] >> 8);
  }
}
// The camera's answer counts when it has a header and starts with "RB".
inline bool ControlReplyValid(const uint8_t* reply, uint32_t length) {
  return length >= 8 && reply[0] == 'R' && reply[1] == 'B';
}

// A frame and its sequence, handed from the sensor thread to the emulated camera.
struct Slot {
  std::mutex mutex;
  std::vector<uint8_t> data;
  uint32_t sequence = 0;
  void Publish(const uint8_t* frame, size_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    data.assign(frame, frame + size);
    sequence = sequence == 0xFFFFFFFEu ? 1 : sequence + 1;  // never 0: 0 is "nothing new"
  }
  // The newest frame when it is not `after`; 0 when there is none or nothing new.
  uint32_t Copy(uint32_t after, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!sequence || sequence == after) return 0;
    out = data;
    return sequence;
  }
};

// One stream's frame, reassembled from the camera's isochronous packets: a 12-byte header
// ("RB", flags: stream in the high nibble (0x70 depth, 0x80 colour), 1 start, 5 end), then
// data. Returns true when a packet ends a complete frame.
struct StreamAssembly {
  uint8_t flag;
  std::vector<uint8_t> frame;
  size_t fill = 0;
  bool in_frame = false;
  uint32_t complete = 0, incomplete = 0;
  StreamAssembly(uint8_t stream_flag, size_t frame_bytes) : flag(stream_flag), frame(frame_bytes) {}
  bool Packet(const uint8_t* packet, uint32_t length) {
    if (length < 12 || length > kPacketBytes) return false;
    if (packet[0] != 'R' || packet[1] != 'B') return false;
    if ((packet[3] & 0xF0) != flag) return false;
    const uint32_t kind = packet[3] & 0x0F, body = length - 12;
    if (kind == 1) {
      in_frame = true;
      fill = 0;
    } else if (!in_frame) {
      return false;
    }
    if (fill + body <= frame.size()) {
      std::memcpy(frame.data() + fill, packet + 12, body);
      fill += body;
    } else if (kind != 5) {
      return false;
    }
    if (kind != 5) return false;
    in_frame = false;
    if (fill != frame.size()) {
      ++incomplete;
      return false;
    }
    ++complete;
    return true;
  }
};

// The direct USB path's operating-system calls.
struct SystemUsb {
  // The first /dev/ugen* node whose device descriptor is a Kinect camera's, else "".
  static std::string FindCamera() {
    std::string found;
    DIR* directory = opendir("/dev");
    if (!directory) return found;
    while (found.empty()) {
      const dirent* entry = readdir(directory);
      if (!entry) break;
      if (std::strncmp(entry->d_name, "ugen", 4)) continue;
      const std::string path = std::string("/dev/") + entry->d_name;
      const int fd = open(path.c_str(), O_NONBLOCK);
      if (fd < 0) continue;
      ugen::DeviceDescriptor descriptor{};
      if (!ioctl(fd, ugen::kGetDeviceDesc, &descriptor) &&
          IsKinectCamera(uint16_t(descriptor.idVendor[0] | descriptor.idVendor[1] << 8),
                         uint16_t(descriptor.idProduct[0] | descriptor.idProduct[1] << 8)))
        found = path;
      close(fd);
    }
    closedir(directory);
    return found;
  }
  static int Open(const std::string& path) { return open(path.c_str(), O_RDWR); }
  static void Close(int fd) { close(fd); }
  static int Ioctl(int fd, unsigned long request, void* argument) {
    return ioctl(fd, request, argument);
  }
  static void Wait(int fd, int milliseconds) {
    pollfd poll_fd{fd, POLLIN | POLLOUT | POLLRDNORM, 0};
    poll(&poll_fd, 1, milliseconds);
  }
};

template <typename Usb = SystemUsb>
class BasicSensor {
 public:
  BasicSensor() { thread_ = std::thread([this] { Run(); }); }
  ~BasicSensor() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }
  BasicSensor(const BasicSensor&) = delete;
  BasicSensor& operator=(const BasicSensor&) = delete;

  // A source (the relay or the camera itself) is delivering frames.
  bool active() const { return active_; }
  uint32_t CopyDepth(uint32_t after, std::vector<uint8_t>& out) { return depth_.Copy(after, out); }
  uint32_t CopyColor(uint32_t after, std::vector<uint8_t>& out) { return colour_.Copy(after, out); }

 private:
  static void Sleep(int64_t nanoseconds) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
  }
  static int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void Run() {
    for (int attempt = 0; running_; --attempt) {
      if (Relay()) continue;
      const std::string path = Usb::FindCamera();
      if (!path.empty()) {
        const int fd = Usb::Open(path);
        if (fd >= 0) {
          XELOGW("Kinect USB: camera {} opened directly", path);
          Session(fd);
          Usb::Close(fd);
        } else if (!attempt || errno != last_errno_) {
          last_errno_ = errno;
          XELOGW("Kinect USB: camera {} found but open failed (errno {}); trying the relay on "
                 "127.0.0.1:{}",
                 path, last_errno_, kRelayPort);
        }
      }
      if (running_ && Relay()) continue;
      if (!attempt && path.empty())
        XELOGI("Kinect USB: no camera on USB and no relay; using pose-derived frames");
      for (int step = 0; step < 20 && running_; ++step) Sleep(100'000'000);
    }
    active_ = false;
  }

  bool ReceiveAll(int fd, uint8_t* at, size_t size) {
    while (size) {
      if (!running_) return false;
      const ssize_t got = recv(fd, at, size, 0);
      if (got > 0) {
        at += got;
        size -= size_t(got);
      } else if (!got || (errno != EAGAIN && errno != EINTR)) {
        return false;
      }
    }
    return true;
  }

  // Frames from kinect-tracker.elf's relay. False when it does not accept a connection.
  bool Relay() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    auto* port = reinterpret_cast<uint8_t*>(&address.sin_port);
    port[0] = uint8_t(kRelayPort >> 8);
    port[1] = uint8_t(kRelayPort);
    const uint8_t loopback[4] = {127, 0, 0, 1};
    std::memcpy(&address.sin_addr, loopback, 4);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) {
      close(fd);
      return false;
    }
    // The receive wakes up every second, so a stop does not wait for a frame.
    const timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    XELOGW("Kinect USB: receiving camera frames from the relay on 127.0.0.1:{}", kRelayPort);
    active_ = true;
    std::vector<uint8_t> frame(kDepthFrameBytes);
    uint32_t frames = 0;
    while (running_) {
      uint8_t header[12];
      if (!ReceiveAll(fd, header, sizeof(header)) || std::memcmp(header, "KNCT", 4)) break;
      const bool depth = header[4] == 1;
      const uint32_t size = uint32_t(header[8]) | uint32_t(header[9]) << 8 |
                            uint32_t(header[10]) << 16 | uint32_t(header[11]) << 24;
      if (size != (depth ? kDepthFrameBytes : kColorFrameBytes)) break;
      if (!ReceiveAll(fd, frame.data(), size)) break;
      (depth ? depth_ : colour_).Publish(frame.data(), size);
      ++frames;
    }
    active_ = false;
    close(fd);
    XELOGW("Kinect USB: relay closed after {} frames", frames);
    return true;
  }

  // One control command; 0 when the camera answered, else a negative errno or -5.
  int WriteRegister(int fd, uint16_t reg, uint16_t value) {
    uint8_t command[12];
    WriteRegisterCommand(tag_, reg, value, command);
    ugen::CtlRequest request{};
    request.ucr_data = command;
    request.ucr_flags = ugen::kShortTransferOk;
    request.ucr_request.bmRequestType = 0x40;
    request.ucr_request.wLength[0] = sizeof(command);
    if (Usb::Ioctl(fd, ugen::kDoRequest, &request) && errno) return -errno;
    uint8_t reply[512];
    uint32_t length = 0;
    for (int tries = 500; tries; --tries) {
      ugen::CtlRequest answer{};
      answer.ucr_data = reply;
      answer.ucr_flags = ugen::kShortTransferOk;
      answer.ucr_request.bmRequestType = 0xC0;
      answer.ucr_request.wLength[1] = sizeof(reply) >> 8;
      if (Usb::Ioctl(fd, ugen::kDoRequest, &answer)) {
        if (errno) return -errno;
      } else {
        length = answer.ucr_actlen;
      }
      if (length != 0 && length != sizeof(reply)) break;
      Sleep(2'000'000);
    }
    ++tag_;
    return ControlReplyValid(reply, length) ? 0 : -5;
  }

  // The camera opened directly: both streams until a stop, an error or an unplug.
  void Session(int fd) {
    if (WriteRegister(fd, 0x105, 0)) {  // projector auto-cycle off: proves the control path
      XELOGW("Kinect USB: camera does not answer control commands");
      return;
    }
    WriteRegister(fd, 0x06, 0);  // depth off
    WriteRegister(fd, 0x05, 0);  // colour off
    ugen::FsEndpoint endpoints[kEndpoints]{};
    std::vector<uint8_t> buffers[kEndpoints];
    void* pointers[kEndpoints][kPackets];
    uint32_t lengths[kEndpoints][kPackets];
    ugen::FsInit init{endpoints, kEndpoints};
    if (Usb::Ioctl(fd, ugen::kFsInit, &init)) {
      XELOGW("Kinect USB: FS_INIT errno {}", errno);
      return;
    }
    int failed = 0;
    for (uint8_t index = 0; index < kEndpoints; ++index) {
      buffers[index].assign(kTransferBytes, 0);
      ugen::FsOpen request{};
      request.max_bufsize = kTransferBytes;
      request.max_frames = kPackets;
      request.ep_index = index;
      request.ep_no = index < 2 ? 0x82 : 0x81;
      for (uint32_t n = 0; n < kPackets; ++n) pointers[index][n] = &buffers[index][n * kPacketBytes];
      if ((failed = Usb::Ioctl(fd, ugen::kFsOpen, &request))) {
        XELOGW("Kinect USB: FS_OPEN {} errno {}", int(index), errno);
        break;
      }
      endpoints[index].ppBuffer = pointers[index];
      endpoints[index].pLength = lengths[index];
    }
    StreamAssembly depth(0x70, kDepthFrameBytes), colour(0x80, kColorFrameBytes);
    if (!failed) {
      auto submit = [&](uint8_t index) {
        for (auto& length : lengths[index]) length = kPacketBytes;
        endpoints[index].nFrames = kPackets;
        endpoints[index].flags = ugen::kFsShortOk;
        endpoints[index].timeout = ugen::kFsTimeout;
        uint8_t argument = index;
        return Usb::Ioctl(fd, ugen::kFsStart, &argument);
      };
      for (uint8_t index = 0; index < kEndpoints; ++index) submit(index);
      // libfreenect's start_depth (11-bit) and colour (Bayer, 640x480, 30 fps, no mirror).
      WriteRegister(fd, 0x12, 3);
      WriteRegister(fd, 0x13, 1);
      WriteRegister(fd, 0x14, 30);
      WriteRegister(fd, 0x06, 2);
      WriteRegister(fd, 0x17, 0);
      WriteRegister(fd, 0x0C, 0);
      WriteRegister(fd, 0x0D, 1);
      WriteRegister(fd, 0x0E, 30);
      WriteRegister(fd, 0x05, 1);
      WriteRegister(fd, 0x47, 0);
      active_ = true;
      XELOGW("Kinect USB: depth and colour streams started");
      int64_t report = Now() + 10'000'000'000;
      while (running_) {
        Usb::Wait(fd, 50);
        bool stopped = false;
        for (;;) {
          uint8_t index = 0;
          if (Usb::Ioctl(fd, ugen::kFsComplete, &index)) {
            stopped = errno != EBUSY;
            break;
          }
          if (index >= kEndpoints) continue;
          const auto& endpoint = endpoints[index];
          if (!endpoint.status && endpoint.aFrames) {
            StreamAssembly& stream = index < 2 ? depth : colour;
            for (uint32_t n = 0; n < endpoint.aFrames && n < kPackets; ++n)
              if (stream.Packet(&buffers[index][n * kPacketBytes], lengths[index][n]))
                (index < 2 ? depth_ : colour_).Publish(stream.frame.data(), stream.frame.size());
          }
          if (submit(index)) {
            stopped = true;
            break;
          }
        }
        if (stopped) {
          XELOGW("Kinect USB: stream stopped (errno {}), sensor unplugged?", errno);
          break;
        }
        if (Now() >= report) {
          report += 60'000'000'000;
          XELOGI("Kinect USB: frames depth {} colour {} (incomplete {}/{})", depth.complete,
                 colour.complete, depth.incomplete, colour.incomplete);
        }
      }
      active_ = false;
      for (uint8_t index = 0; index < kEndpoints; ++index) {
        uint8_t argument = index;
        Usb::Ioctl(fd, ugen::kFsStop, &argument);
      }
      Sleep(50'000'000);
      for (uint8_t index = 0; index < kEndpoints; ++index) {
        uint8_t argument = index;
        Usb::Ioctl(fd, ugen::kFsClose, &argument);
      }
    }
    uint8_t uninit = 0;
    Usb::Ioctl(fd, ugen::kFsUninit, &uninit);
    WriteRegister(fd, 0x06, 0);
    WriteRegister(fd, 0x05, 0);
    XELOGW("Kinect USB: streams stopped, camera released");
  }

  // Order and sizes as in the official object (depth 0x00, colour 0x28, flags 0x50-0x54).
  Slot depth_, colour_;
  std::atomic<bool> running_{true}, active_{false};
  uint16_t tag_ = 0;
  int last_errno_ = 0;
  std::thread thread_;
};

using Sensor = BasicSensor<>;

}  // namespace xbox360ps5::kinect_usb
