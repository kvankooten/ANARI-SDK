// Copyright 2023 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include <anari/anari_cpp.hpp>
#include <functional>
#include <iostream>
#include <sstream>
#include "ArrayInfo.h"
#include "Buffer.h"
#include "Compression.h"
#include "Logging.h"
#include "async/connection.h"
#include "async/connection_manager.h"
#include "async/work_queue.h"
#include "common.h"

using namespace std::placeholders;

// Global variables
static std::string g_libraryType = "environment";
static ANARILibrary g_library = nullptr;
static bool g_verbose = false;
static unsigned short g_port = 31050;

namespace remote {

void statusFunc(const void *userData,
    ANARIDevice device,
    ANARIObject source,
    ANARIDataType sourceType,
    ANARIStatusSeverity severity,
    ANARIStatusCode code,
    const char *message)
{
  bool verbose = *(const bool *)userData;
  if (severity == ANARI_SEVERITY_FATAL_ERROR) {
    fprintf(stderr, "[FATAL] %s\n", message);
  } else if (severity == ANARI_SEVERITY_ERROR) {
    fprintf(stderr, "[ERROR] %s\n", message);
  } else if (severity == ANARI_SEVERITY_WARNING) {
    fprintf(stderr, "[WARN ] %s\n", message);
  }

  if (!verbose)
    return;

  if (severity == ANARI_SEVERITY_PERFORMANCE_WARNING) {
    fprintf(stderr, "[PERF ] %s\n", message);
  } else if (severity == ANARI_SEVERITY_INFO) {
    fprintf(stderr, "[INFO ] %s\n", message);
  } else if (severity == ANARI_SEVERITY_DEBUG) {
    fprintf(stderr, "[DEBUG] %s\n", message);
  }
}

static ANARIObject newObject(
    ANARIDevice dev, ANARIDataType type, std::string subtype)
{
  if (type == ANARI_LIGHT) {
    return anariNewLight(dev, subtype.c_str());
  } else if (type == ANARI_CAMERA) {
    return anariNewCamera(dev, subtype.c_str());
  } else if (type == ANARI_GEOMETRY) {
    return anariNewGeometry(dev, subtype.c_str());
  } else if (type == ANARI_SPATIAL_FIELD) {
    return anariNewSpatialField(dev, subtype.c_str());
  } else if (type == ANARI_SURFACE) {
    return anariNewSurface(dev);
  } else if (type == ANARI_VOLUME) {
    return anariNewVolume(dev, subtype.c_str());
  } else if (type == ANARI_MATERIAL) {
    return anariNewMaterial(dev, subtype.c_str());
  } else if (type == ANARI_SAMPLER) {
    return anariNewSampler(dev, subtype.c_str());
  } else if (type == ANARI_GROUP) {
    return anariNewGroup(dev);
  } else if (type == ANARI_INSTANCE) {
    return anariNewInstance(dev, subtype.c_str());
  } else if (type == ANARI_WORLD) {
    return anariNewWorld(dev);
  } else if (type == ANARI_FRAME) {
    return anariNewFrame(dev);
  } else if (type == ANARI_RENDERER) {
    return anariNewRenderer(dev, subtype.c_str());
  }

  return nullptr;
}

static ANARIArray newArray(
    ANARIDevice dev, const ArrayInfo &info, const uint8_t *data)
{
  ANARIArray array = nullptr;
  if (info.type == ANARI_ARRAY1D) {
    array = anariNewArray1D(
        dev, nullptr, nullptr, nullptr, info.elementType, info.numItems1);
  } else if (info.type == ANARI_ARRAY2D) {
    array = anariNewArray2D(dev,
        nullptr,
        nullptr,
        nullptr,
        info.elementType,
        info.numItems1,
        info.numItems2);
  } else if (info.type == ANARI_ARRAY3D) {
    array = anariNewArray3D(dev,
        nullptr,
        nullptr,
        nullptr,
        info.elementType,
        info.numItems1,
        info.numItems2,
        info.numItems3);
  }

  if (array && data) {
    void *ptr = anariMapArray(dev, array);
    memcpy(ptr, data, info.getSizeInBytes());
    anariUnmapArray(dev, array);
  }

  return array;
}

struct ServerObject
{
  ANARIDevice device{nullptr};
  ANARIObject handle{nullptr};
  ANARIDataType type{ANARI_UNKNOWN};
};

struct ResourceManager
{
  // Device handles are generated by us and returned to the client
  Handle registerDevice(ANARIDevice dev)
  {
    Handle handle = nextDeviceHandle++;
    size_t newNumHandles = std::max(anariDevices.size(), (size_t)handle + 1);
    anariDevices.resize(newNumHandles);
    serverObjects.resize(newNumHandles);
    serverArrays.resize(newNumHandles);
    anariDevices[handle] = dev;
    return handle;
  }

  // Object handles are generated by the clients
  void registerObject(uint64_t deviceID,
      uint64_t objectID,
      ANARIObject anariObj,
      ANARIDataType type)
  {
    size_t newNumHandles =
        std::max(serverObjects[deviceID].size(), (size_t)objectID + 1);

    serverObjects[deviceID].resize(newNumHandles);
    serverObjects[deviceID][objectID] = {
        anariDevices[deviceID], anariObj, type};
  }

  // Like registerObject, but stores array size; so we can later
  // send the whole array data back to the client on mapArray()
  void registerArray(uint64_t deviceID,
      uint64_t objectID,
      ANARIObject anariObj,
      const ArrayInfo &info)
  {
    registerObject(deviceID, objectID, anariObj, info.type);

    size_t newNumHandles =
        std::max(serverArrays[deviceID].size(), (size_t)objectID + 1);

    serverArrays[deviceID].resize(newNumHandles);
    serverArrays[deviceID][objectID] = info;
  }

  ANARIDevice getDevice(uint64_t deviceID)
  {
    if (deviceID >= anariDevices.size())
      return nullptr;

    return anariDevices[deviceID];
  }

  ServerObject getServerObject(Handle deviceHandle, Handle objectHandle)
  {
    if (deviceHandle >= serverObjects.size()
        || objectHandle >= serverObjects[deviceHandle].size())
      return {};

    return serverObjects[deviceHandle][objectHandle];
  }

  ArrayInfo getArrayInfo(Handle deviceHandle, Handle objectHandle)
  {
    if (deviceHandle >= serverArrays.size()
        || objectHandle >= serverArrays[deviceHandle].size())
      return {};

    return serverArrays[deviceHandle][objectHandle];
  }

  Handle nextDeviceHandle = 1;

  std::vector<ANARIDevice> anariDevices;

  // vector of anari objects per device
  std::vector<std::vector<ServerObject>> serverObjects;

  // vector of array infos per device
  std::vector<std::vector<ArrayInfo>> serverArrays;
};

struct Server
{
  // For now only one client:
  struct
  {
    CompressionFeatures compression;
  } client;

  ResourceManager resourceManager;
  async::connection_manager_pointer manager;
  async::connection_pointer conn;
  async::work_queue queue;

  explicit Server(unsigned short port = 31050)
      : manager(async::make_connection_manager(port))
  {
    logging::Initialize();

    g_library = anariLoadLibrary(g_libraryType.c_str(), statusFunc, &g_verbose);
  }

  ~Server()
  {
    anariUnloadLibrary(g_library);
  }

  void accept()
  {
    LOG(logging::Level::Info) << "Server: accepting...";

    manager->accept(std::bind(&Server::handleNewConnection,
        this,
        std::placeholders::_1,
        std::placeholders::_2));
  }

  void run()
  {
    manager->run_in_thread();
    queue.run_in_thread();
  }

  void wait()
  {
    manager->wait();
  }

  void write(unsigned type, std::shared_ptr<Buffer> buf)
  {
    queue.post(std::bind(&Server::writeImpl, this, type, buf));
  }

  void writeImpl(unsigned type, std::shared_ptr<Buffer> buf)
  {
    conn->write(type, *buf);
  }

  std::vector<uint8_t> translateArrayData(
      Buffer &buf, ANARIDevice dev, ArrayInfo info)
  {
    std::vector<uint8_t> arrayData(info.getSizeInBytes());
    buf.read((char *)arrayData.data(), arrayData.size());

    // Translate remote to device handles
    if (anari::isObject(info.elementType)) {
      const auto &serverObjects = resourceManager.serverObjects[(uint64_t)dev];

      size_t numObjects = info.numItems1 * std::max(uint64_t(1), info.numItems2)
          * std::max(uint64_t(1), info.numItems3);

      // This only works b/c sizeof(ANARIObject)==sizeof(uint64_t)!
      // TODO: can this cause issues with alignment on some platforms?!
      const uint64_t *handles = (const uint64_t *)arrayData.data();
      ANARIObject *objects = (ANARIObject *)arrayData.data();

      for (size_t i = 0; i < numObjects; ++i) {
        objects[i] = serverObjects[handles[i]].handle;
      }
    }
    return arrayData;
  }

  bool handleNewConnection(
      async::connection_pointer new_conn, boost::system::error_code const &e)
  {
    if (e) {
      LOG(logging::Level::Error)
          << "Server: could not connect to client: " << e.message();
      manager->stop();
      return false;
    }

    LOG(logging::Level::Info) << "server: connected";

    // Accept and save this connection
    // and set the message handler of the connection
    conn = new_conn;
    conn->set_handler(std::bind(&Server::handleMessage,
        this,
        std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3));

    // Accept new connections (TODO: the new connection
    // will overwrite the current one, so, store these
    // in a list of connections)
    accept();

    return true;
  }

  void handleMessage(async::connection::reason reason,
      async::message_pointer message,
      boost::system::error_code const &e)
  {
    if (e) {
      LOG(logging::Level::Error) << "Server: error: " << e.message();

      manager->stop();
      return;
    }

    LOG(logging::Level::Info) << "Message: " << toString(message->type())
        << ", message size: " << prettyBytes(message->size());

    if (reason == async::connection::Read) {
      if (message->type() == MessageType::NewDevice) {
        const char *msg = message->data();

        int32_t len = *(int32_t *)msg;
        msg += sizeof(len);

        std::string deviceType(msg, len);
        msg += len;

        client.compression = *(CompressionFeatures *)msg;
        msg += sizeof(CompressionFeatures);

        ANARIDevice dev = anariNewDevice(g_library, deviceType.c_str());
        Handle deviceHandle = resourceManager.registerDevice(dev);
        CompressionFeatures cf = getCompressionFeatures();

        // return device handle and other info to client
        auto buf = std::make_shared<Buffer>();
        buf->write(deviceHandle);
        buf->write(cf);
        write(MessageType::DeviceHandle, buf);

        LOG(logging::Level::Info)
            << "Creating new device, type: " << deviceType
            << ", device ID: " << deviceHandle << ", ANARI handle: " << dev;
        LOG(logging::Level::Info)
            << "Client has TurboJPEG: " << client.compression.hasTurboJPEG;
        LOG(logging::Level::Info)
            << "Client has SNAPPY: " << client.compression.hasSNAPPY;
      } else if (message->type() == MessageType::NewObject) {
        Buffer buf(message->data(), message->size());

        ANARIDevice deviceHandle;
        buf.read(deviceHandle);

        ANARIDataType type;
        buf.read(type);

        std::string subtype;
        buf.read(subtype);

        uint64_t objectID;
        buf.read(objectID);

        ANARIDevice dev = resourceManager.getDevice((uint64_t)deviceHandle);
        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }
        ANARIObject anariObj = newObject(dev, type, subtype);
        resourceManager.registerObject(
            (uint64_t)deviceHandle, objectID, anariObj, type);

        LOG(logging::Level::Info)
            << "Creating new object, objectID: " << objectID
            << ", ANARI handle: " << anariObj;
      } else if (message->type() == MessageType::NewArray) {
        Buffer buf(message->data(), message->size());

        ANARIDevice deviceHandle;
        buf.read(deviceHandle);

        ArrayInfo info;
        buf.read(info.type);

        uint64_t objectID;
        buf.read(objectID);
        buf.read(info.elementType);
        buf.read(info.numItems1);
        buf.read(info.numItems2);
        buf.read(info.numItems3);

        ANARIDevice dev = resourceManager.getDevice((uint64_t)deviceHandle);
        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }

        std::vector<uint8_t> arrayData;
        if (buf.pos < message->size()) {
          arrayData = translateArrayData(buf, deviceHandle, info);
        }

        ANARIArray anariArr = newArray(dev, info, arrayData.data());
        resourceManager.registerArray(
            (uint64_t)deviceHandle, objectID, anariArr, info);

        LOG(logging::Level::Info)
            << "Creating new array, objectID: " << objectID
            << ", ANARI handle: " << anariArr;
      } else if (message->type() == MessageType::SetParam) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!dev || !serverObj.handle) {
          LOG(logging::Level::Error)
              << "Error setting param on object. Handle: " << objectHandle;
          // manager->stop(); // legal?
          return;
        }

        std::string name;
        buf.read(name);

        ANARIDataType parmType;
        buf.read(parmType);

        std::vector<char> parmValue;
        if (anari::isObject(parmType)) {
          parmValue.resize(sizeof(uint64_t));
          buf.read((char *)parmValue.data(), sizeof(uint64_t));
        } else {
          parmValue.resize(anari::sizeOf(parmType));
          buf.read((char *)parmValue.data(), anari::sizeOf(parmType));
        }

        if (anari::isObject(parmType)) {
          const auto &serverObjects =
              resourceManager.serverObjects[(uint64_t)deviceHandle];
          Handle hnd = *(Handle *)parmValue.data();
          anariSetParameter(dev,
              serverObj.handle,
              name.c_str(),
              parmType,
              &serverObjects[hnd].handle);

          LOG(logging::Level::Info)
              << "Set param \"" << name << "\" on object: " << objectHandle
              << ", param is an object. Handle: " << hnd
              << ", ANARI handle: " << serverObjects[hnd].handle;
        } else {
          anariSetParameter(
              dev, serverObj.handle, name.c_str(), parmType, parmValue.data());

          LOG(logging::Level::Info)
              << "Set param \"" << name << "\" on object: " << objectHandle;
        }
      } else if (message->type() == MessageType::UnsetParam) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!dev || !serverObj.handle) {
          LOG(logging::Level::Error)
              << "Error setting param on object. Handle: " << objectHandle;
          // manager->stop(); // legal?
          return;
        }

        std::string name;
        buf.read(name);

        anariUnsetParameter(dev, serverObj.handle, name.c_str());
      } else if (message->type() == MessageType::UnsetAllParams) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!dev || !serverObj.handle) {
          LOG(logging::Level::Error)
              << "Error unsetting all params on object. Handle: "
              << objectHandle;
          // manager->stop(); // legal?
          return;
        }

        anariUnsetAllParameters(dev, serverObj.handle);
      } else if (message->type() == MessageType::CommitParams) {
        Buffer buf(message->data(), message->size());

        if (message->size() == sizeof(Handle)) {
          // handle only => commit params of the device itself!
          ANARIDevice device;
          buf.read((char *)&device, sizeof(device));

          ANARIDevice dev = resourceManager.getDevice((uint64_t)device);

          if (!dev) {
            LOG(logging::Level::Error)
                << "Error committing devcie params: " << dev;
            // manager->stop(); // legal?
            return;
          }

          anariCommitParameters(dev, dev);
        } else {
          Handle deviceHandle, objectHandle;
          buf.read(deviceHandle);
          buf.read(objectHandle);

          ANARIDevice dev = resourceManager.getDevice(deviceHandle);

          ServerObject serverObj =
              resourceManager.getServerObject(deviceHandle, objectHandle);

          if (!dev || !serverObj.handle) {
            LOG(logging::Level::Error)
                << "Error setting param on object. Handle: " << objectHandle;
            // manager->stop(); // legal?
            return;
          }

          anariCommitParameters(dev, serverObj.handle);

          LOG(logging::Level::Info)
              << "Committed object. Handle: " << objectHandle;
        }
      } else if (message->type() == MessageType::Release) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!dev || !serverObj.handle) {
          LOG(logging::Level::Error)
              << "Error releasing object. Handle: " << objectHandle;
          // manager->stop(); // legal?
          return;
        }

        anariRelease(dev, serverObj.handle);

        LOG(logging::Level::Info)
            << "Released object. Handle: " << objectHandle;
      } else if (message->type() == MessageType::Retain) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!dev || !serverObj.handle) {
          LOG(logging::Level::Error)
              << "Error retaining object. Handle: " << objectHandle;
          // manager->stop(); // legal?
          return;
        }

        anariRetain(dev, serverObj.handle);

        LOG(logging::Level::Info)
            << "Retained object. Handle: " << objectHandle;
      } else if (message->type() == MessageType::MapArray) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!dev || !serverObj.handle) {
          LOG(logging::Level::Error)
              << "Error retaining object. Handle: " << objectHandle;
          // manager->stop(); // legal?
          return;
        }

        void *ptr = anariMapArray(dev, (ANARIArray)serverObj.handle);

        const ArrayInfo &info =
            resourceManager.getArrayInfo(deviceHandle, objectHandle);

        uint64_t numBytes = info.getSizeInBytes();

        auto outbuf = std::make_shared<Buffer>();
        outbuf->write(objectHandle);
        outbuf->write(numBytes);
        outbuf->write((const char *)ptr, numBytes);
        write(MessageType::ArrayMapped, outbuf);

        LOG(logging::Level::Info) << "Mapped array. Handle: " << objectHandle;
      } else if (message->type() == MessageType::UnmapArray) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!dev || !serverObj.handle) {
          LOG(logging::Level::Error)
              << "Error retaining object. Handle: " << objectHandle;
          // manager->stop(); // legal?
          return;
        }

        // Array is currently mapped - unmap
        anariUnmapArray(dev, (ANARIArray)serverObj.handle);

        // Now map so we can write to it
        void *ptr = anariMapArray(dev, (ANARIArray)serverObj.handle);

        // Fetch data into separate buffer and copy
        std::vector<uint8_t> arrayData;
        if (buf.pos < message->size()) {
          ArrayInfo info =
              resourceManager.getArrayInfo(deviceHandle, objectHandle);
          arrayData = translateArrayData(buf, (ANARIDevice)deviceHandle, info);
          memcpy(ptr, arrayData.data(), arrayData.size());
        }

        // Unmap again..
        anariUnmapArray(dev, (ANARIArray)serverObj.handle);

        auto outbuf = std::make_shared<Buffer>();
        outbuf->write(objectHandle);
        write(MessageType::ArrayUnmapped, outbuf);

        LOG(logging::Level::Info) << "Unmapped array. Handle: " << objectHandle;
      } else if (message->type() == MessageType::RenderFrame) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }

        ANARIFrame frame = (ANARIFrame)resourceManager
                               .getServerObject(deviceHandle, objectHandle)
                               .handle;
        anariRenderFrame(dev, frame);

        // Block and send image over the wire
        anariFrameReady(dev, frame, ANARI_WAIT);

        CompressionFeatures cf = getCompressionFeatures();

        uint32_t width, height;
        ANARIDataType type;
        const char *color = (const char *)anariMapFrame(
            dev, frame, "channel.color", &width, &height, &type);
        size_t colorSize =
            type == ANARI_UNKNOWN ? 0 : width * height * anari::sizeOf(type);
        if (color != nullptr && colorSize != 0) {
          auto outbuf = std::make_shared<Buffer>();
          outbuf->write(objectHandle);
          outbuf->write(width);
          outbuf->write(height);
          outbuf->write(type);

          bool compressionTurboJPEG =
              cf.hasTurboJPEG && client.compression.hasTurboJPEG;

          if (compressionTurboJPEG
              && type == ANARI_UFIXED8_RGBA_SRGB) { // TODO: more formats..
            TurboJPEGOptions options;
            options.width = width;
            options.height = height;
            options.pixelFormat = TurboJPEGOptions::PixelFormat::RGBX;
            options.quality = 80;

            std::vector<uint8_t> compressed(
                getMaxCompressedBufferSizeTurboJPEG(options));

            if (compressed.size() != 0) {
              size_t compressedSize;
              if (compressTurboJPEG((const uint8_t *)color,
                      compressed.data(),
                      compressedSize,
                      options)) {
                uint32_t compressedSize32(compressedSize);
                outbuf->write(compressedSize32);
                outbuf->write((const char *)compressed.data(), compressedSize);

                LOG(logging::Level::Info) << "turbojpeg compression size: "
                                          << prettyBytes(compressedSize);
              }
            }
          } else {
            outbuf->write(color, colorSize);
          }
          write(MessageType::ChannelColor, outbuf);
        }

        const char *depth = (const char *)anariMapFrame(
            dev, frame, "channel.depth", &width, &height, &type);
        size_t depthSize =
            type == ANARI_UNKNOWN ? 0 : width * height * anari::sizeOf(type);
        if (depth != nullptr && depthSize != 0) {
          auto outbuf = std::make_shared<Buffer>();
          outbuf->write(objectHandle);
          outbuf->write(width);
          outbuf->write(height);
          outbuf->write(type);

          bool compressionSNAPPY = cf.hasSNAPPY && client.compression.hasSNAPPY;

          if (compressionSNAPPY && type == ANARI_FLOAT32) {
            SNAPPYOptions options;
            options.inputSize = depthSize;

            std::vector<uint8_t> compressed(
                getMaxCompressedBufferSizeSNAPPY(options));

            size_t compressedSize = 0;

            compressSNAPPY((const uint8_t *)depth,
                compressed.data(),
                compressedSize,
                options);

            uint32_t compressedSize32(compressedSize);
            outbuf->write(compressedSize32);
            outbuf->write((const char *)compressed.data(), compressedSize);
          } else {
            outbuf->write(depth, depthSize);
          }
          write(MessageType::ChannelDepth, outbuf);
        }

        LOG(logging::Level::Info)
            << "Frame rendered. Object handle: " << objectHandle;
      } else if (message->type() == MessageType::FrameReady) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }

        ANARIWaitMask waitMask;
        buf.read(waitMask);

        ANARIFrame frame = (ANARIFrame)resourceManager
                               .getServerObject(deviceHandle, objectHandle)
                               .handle;
        anariFrameReady(dev, frame, waitMask);

        auto outbuf = std::make_shared<Buffer>();
        outbuf->write(objectHandle);
        write(MessageType::FrameIsReady, outbuf);

        LOG(logging::Level::Info) << "Signal frame is ready to client";
      } else if (message->type() == MessageType::GetProperty) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle, objectHandle;
        buf.read(deviceHandle);
        buf.read(objectHandle);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }

        ServerObject serverObj =
            resourceManager.getServerObject(deviceHandle, objectHandle);

        if (!serverObj.handle) { // then we're querying properties on the device
                                 // itself!
          serverObj.device = dev;
          serverObj.handle = dev;
          serverObj.type = ANARI_DEVICE;
        }

        std::string name;
        buf.read(name);

        ANARIDataType type;
        buf.read(type);

        uint64_t size;
        buf.read(size);

        ANARIWaitMask mask;
        buf.read(mask);

        auto outbuf = std::make_shared<Buffer>();

        if (type == ANARI_STRING_LIST) {
          const char *const *value = nullptr;
          int result = anariGetProperty(
              dev, serverObj.handle, name.data(), type, &value, size, mask);

          outbuf->write(objectHandle);
          outbuf->write(name);
          outbuf->write(result);

          StringList stringList((const char **)value);
          outbuf->write(stringList);
        } else if (type == ANARI_DATA_TYPE_LIST) {
          throw std::runtime_error(
              "getProperty with ANARI_DATA_TYPE_LIST not implemented yet!");
        } else { // POD!
          std::vector<char> mem(size);

          int result = anariGetProperty(
              dev, serverObj.handle, name.data(), type, mem.data(), size, mask);

          outbuf->write(objectHandle);
          outbuf->write(name);
          outbuf->write(result);
          outbuf->write((const char *)mem.data(), size);
        }
        write(MessageType::Property, outbuf);
      } else if (message->type() == MessageType::GetObjectSubtypes) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle;
        buf.read(deviceHandle);

        ANARIDataType objectType;
        buf.read(objectType);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }

        auto outbuf = std::make_shared<Buffer>();
        outbuf->write(objectType);

        const char **subtypes = anariGetObjectSubtypes(dev, objectType);

        StringList stringList(subtypes);
        outbuf->write(stringList);

        write(MessageType::ObjectSubtypes, outbuf);
      } else if (message->type() == MessageType::GetObjectInfo) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle;
        buf.read(deviceHandle);

        ANARIDataType objectType;
        buf.read(objectType);

        std::string objectSubtype;
        buf.read(objectSubtype);

        std::string infoName;
        buf.read(infoName);

        ANARIDataType infoType;
        buf.read(infoType);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }

        auto outbuf = std::make_shared<Buffer>();
        outbuf->write(objectType);
        outbuf->write(std::string(objectSubtype));
        outbuf->write(std::string(infoName));
        outbuf->write(infoType);

        const void *info = anariGetObjectInfo(
            dev, objectType, objectSubtype.data(), infoName.data(), infoType);

        if (info != nullptr) {
          if (infoType == ANARI_STRING) {
            auto *str = (const char *)info;
            outbuf->write(std::string(str));
          } else if (infoType == ANARI_STRING_LIST) {
            StringList stringList((const char **)info);
            outbuf->write(stringList);
          } else if (infoType == ANARI_PARAMETER_LIST) {
            ParameterList parameterList((const Parameter *)info);
            outbuf->write(parameterList);
          } else {
            outbuf->write((const char *)info, anari::sizeOf(infoType));
          }
        }
        write(MessageType::ObjectInfo, outbuf);
      } else if (message->type() == MessageType::GetParameterInfo) {
        Buffer buf(message->data(), message->size());

        Handle deviceHandle;
        buf.read(deviceHandle);

        ANARIDataType objectType;
        buf.read(objectType);

        std::string objectSubtype;
        buf.read(objectSubtype);

        std::string parameterName;
        buf.read(parameterName);

        ANARIDataType parameterType;
        buf.read(parameterType);

        std::string infoName;
        buf.read(infoName);

        ANARIDataType infoType;
        buf.read(infoType);

        ANARIDevice dev = resourceManager.getDevice(deviceHandle);

        if (!dev) {
          LOG(logging::Level::Error)
              << "Server: invalid device: " << deviceHandle;
          // manager->stop(); // legal?
          return;
        }

        auto outbuf = std::make_shared<Buffer>();
        outbuf->write(objectType);
        outbuf->write(objectSubtype);
        outbuf->write(parameterName);
        outbuf->write(parameterType);
        outbuf->write(infoName);
        outbuf->write(infoType);

        const void *info = anariGetParameterInfo(dev,
            objectType,
            objectSubtype.data(),
            parameterName.data(),
            parameterType,
            infoName.data(),
            infoType);

        if (info != nullptr) {
          if (infoType == ANARI_STRING) {
            auto *str = (const char *)info;
            outbuf->write(std::string(str));
          } else if (infoType == ANARI_STRING_LIST) {
            StringList stringList((const char **)info);
            outbuf->write(stringList);
          } else if (infoType == ANARI_PARAMETER_LIST) {
            ParameterList parameterList((const Parameter *)info);
            outbuf->write(parameterList);
          } else {
            outbuf->write((const char *)info, anari::sizeOf(infoType));
          }
        }
        write(MessageType::ParameterInfo, outbuf);
      } else {
        LOG(logging::Level::Warning)
            << "Unhandled message of size: " << message->size();
      }
    } else {
      // LOG(logging::Level::Info) << "written";
    }
  }
};

} // namespace remote

///////////////////////////////////////////////////////////////////////////////

static void printUsage()
{
  std::cout << "./anari-remote-server [{--help|-h}]\n"
            << "   [{--verbose|-v}]\n"
            << "   [{--library|-l} <ANARI library>]\n"
            << "   [{--port|-p} <N>]\n";
}

static void parseCommandLine(int argc, char *argv[])
{
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-v" || arg == "--verbose")
      g_verbose = true;
    if (arg == "--help" || arg == "-h") {
      printUsage();
      std::exit(0);
    } else if (arg == "-l" || arg == "--library")
      g_libraryType = argv[++i];
    else if (arg == "-p" || arg == "--port")
      g_port = std::stoi(argv[++i]);
  }
}

int main(int argc, char *argv[])
{
  parseCommandLine(argc, argv);
  remote::Server srv(g_port);
  srv.accept();
  srv.run();
  srv.wait();
}
