/*
 * RokubiminiSerialImpl.hpp
 *  Created on: April, 2020
 *  Author(s):  Mike Karamousadakis, Ilias Patsiaouras
 */

//! System dependencies
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#include <functional>
#include <thread>
#include <sys/wait.h>
#include <asm/ioctls.h>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <iostream>
#include <fstream>
#include <string>

#include "ros/ros.h"
//! Class header file
#include <rokubimini_serial/RokubiminiSerialImpl.hpp>
#include <rokubimini_serial/RokubiminiSerialCommunication.hpp>

namespace rokubimini
{
namespace serial
{
RokubiminiSerialImpl::RokubiminiSerialImpl(const std::string& name, const std::string& port)
  : name_(name)
  , port_(port)
  , productName_("")
  , serialNumber_(-1)
  , usbFileDescriptor_{ -1 }
  , frameSync_{ false }
  , usbStreamIn_{ nullptr }
  , usbStreamOut_{ nullptr }
  , runInThreadedMode_(true)
  , connectionThread_()
  , pollingThread_()
  , connectionState_(ConnectionState::DISCONNECTED)
  , errorState_(ErrorState::NO_ERROR)
  , modeState_{ ModeState::RUN_MODE }
  , isRunning_{ true }
  , pollingSyncErrorCounter_(0)
  , frameReceivedCounter_(0)
  , frameSuccessCounter_(0)
  , frameCrcErrorCounter_(0)
  , frameSyncErrorCounter_(0)
  , maxFrameSyncErrorCounts_(10000)
  , maxCountOpenSerialPort_(10)
  , frameOffset_(0)
  , readTimeout_(MAXIMUM_ACCEPTABLE_TIMEOUT)
  , timeoutCounter_(0)
  , dataReady_{ false }
  , runsAsync_{ false }
  , frameTimestampOffset_()
  , frameTimestampSecs_(0)
  , frameTimestampOffsetIsValid_{ false }
{
}

bool RokubiminiSerialImpl::init()
{
  ROS_DEBUG("[%s] Attempting to init device.", name_.c_str());

  // Connect to serial port
  if (connect())
  {
    // If running in threaded mode, wait for the connection thread to finish. This either means that connection has been
    // established or failed.
    if (runInThreadedMode_ && connectionThread_.joinable())
    {
      connectionThread_.join();
      if (!isConnected())
      {
        ROS_ERROR("[%s] Could not establish connection with device. Init failed.", name_.c_str());
        return false;
      }
    }
    // Sleep for 2.0 seconds for the sensor to boot and start sending data.
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  else
  {
    ROS_ERROR("[%s] Could not establish connection with device. Init failed.", name_.c_str());
    return false;
  }
  if (!setInitMode())
  {
    ROS_ERROR("[%s] Could not bring device to INIT mode. Init failed.", name_.c_str());
    return false;
  }
  return true;
}

bool RokubiminiSerialImpl::startPollingThread()
{
  // Start the polling thread if driver is to run in threaded mode and
  // if the polling thread is not already running
  if (runInThreadedMode_ && !pollingThread_.joinable())
  {
    ROS_INFO("[%s] Launching polling thread.", name_.c_str());
    pollingThread_ = boost::thread{ &RokubiminiSerialImpl::pollingWorker, this };
  }
  return true;
}

bool RokubiminiSerialImpl::startup()
{
  ROS_INFO_STREAM("[" << getName() << "] "
                      << "The following serial device has been found and initialized:");
  ROS_INFO_STREAM("[" << getName() << "] "
                      << "Port: " << port_);
  ROS_INFO_STREAM("[" << getName() << "] "
                      << "Name: '" << productName_ << "'");
  ROS_INFO_STREAM("[" << getName() << "] "
                      << "S/N: " << serialNumber_);
  return true;
}

void RokubiminiSerialImpl::closeSerialPort()
{
  // Close the serial-port file descriptior
  if (usbFileDescriptor_ != -1)
  {
    ROS_DEBUG("[%s] Closing Serial Communication", name_.c_str());
    close(usbFileDescriptor_);
  }
  usbStreamIn_.close();
  usbStreamOut_.close();
  if (((usbStreamIn_.fail() | usbStreamOut_.fail()) != 0))
  {
    ROS_ERROR("[%s] Failed to close file streams.", name_.c_str());
  }
}

void RokubiminiSerialImpl::shutdown()
{
  ROS_INFO("[%s] Driver will attempt to shut-down", name_.c_str());

  // Signal for termination (all background thread)
  isRunning_ = false;

  if (runsAsync())
  {
    // notify the publishing thread to stop
    newFrameIsAvailable_.notify_all();
  }

  // Shutdown the connection thread if running
  if (runInThreadedMode_ && connectionThread_.joinable())
  {
    connectionThread_.join();
  }

  // Shutdown the polling thread if running
  if (runInThreadedMode_ && pollingThread_.joinable())
  {
    pollingThread_.join();
  }
  closeSerialPort();

  ROS_INFO("[%s] Shut-down successful", name_.c_str());
}

bool RokubiminiSerialImpl::connect()
{
  // Abort attempt if the driver is already in the process of connecting
  if (connectionState_ == ConnectionState::ISCONNECTING)
  {
    return false;
  }
  else
  {
    connectionState_ = ConnectionState::ISCONNECTING;
  }

  // Close the file descriptor if it was already open
  if (usbFileDescriptor_ != -1)
  {
    close(usbFileDescriptor_);
    usbFileDescriptor_ = -1;
  }

  // Reset error counters and error state
  frameReceivedCounter_ = 0;
  frameSuccessCounter_ = 0;
  frameCrcErrorCounter_ = 0;
  frameSyncErrorCounter_ = 0;
  timeoutCounter_ = 0;
  errorState_ = ErrorState::NO_ERROR;

  // Check if the driver is set to run in threaded mode
  if (!runInThreadedMode_)
  {
    return initSensorCommunication(false);
  }
  else
  {
    // Start the connection thread
    connectionThread_ = boost::thread{ &RokubiminiSerialImpl::connectionWorker, this };
  }

  return true;
}

bool RokubiminiSerialImpl::connect(const std::string& port)
{
  port_ = port;
  return connect();
}
void RokubiminiSerialImpl::increaseAndCheckTimeoutCounter()
{
  ++timeoutCounter_;
  ROS_WARN("[%s] Timeout reached and didn't get any valid data from the device.", name_.c_str());
}
bool RokubiminiSerialImpl::readDevice(RxFrame& frame)
{
  uint32_t stored_chars;
  bool read_finished = false;
  timespec t_current, t_previous;
  double time_delta;
  char dummy;
  while (errorState_ == ErrorState::NO_ERROR && isRunning_ && modeState_ == ModeState::RUN_MODE)
  {
    /* peek the next available byte and check if is the header.
     * make sure to consume it after, if it isn't. Using static
     * variable for the sync flag will preserve status of sync
     * between calls */
    time_delta = 0.0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_current);
    while (!frameSync_ && time_delta < readTimeout_)
    {
      uint8_t possible_header;
      /* read bytes 1 by 1 to find the header */
      possible_header = usbStreamIn_.peek();
      if (possible_header == frameHeader)
      {
        /* read the remaining frame to make check also CRC */
        usbStreamIn_.read((char*)&frame.bytes, sizeof(frame));
        if (frame.crc16_ccitt == calcCrc16X25(frame.data.bytes, sizeof(frame.data)))
        {
          ROS_INFO("[%s] Frame synced with 0x%X header", name_.c_str(), frameHeader);
          frameSync_ = true;
          frameOffset_ = 0;
        }
        else
        {
          ROS_WARN("[%s] Skipping incomplete frame", name_.c_str());
        }
      }
      else
      {
        // consume the wrong header
        usbStreamIn_.get(dummy);
      }
      t_previous = t_current;
      clock_gettime(CLOCK_MONOTONIC_RAW, &t_current);
      time_delta += diffSec(t_previous, t_current);
    }
    if (time_delta > readTimeout_)
    {
      increaseAndCheckTimeoutCounter();
      return false;
    }
    /* Read the sensor measurements frame assuming that is alligned with the RX buffer */
    try
    {
      time_delta = 0.0;
      clock_gettime(CLOCK_MONOTONIC_RAW, &t_current);
      ROS_DEBUG("[%s] Frame offset before reading is: %u", name_.c_str(), frameOffset_);
      while (!read_finished && time_delta < readTimeout_)
      {
        stored_chars = usbStreamIn_.readsome((char*)&placeholder_.bytes + frameOffset_, sizeof(RxFrame) - frameOffset_);
        frameOffset_ += stored_chars;
        if (stored_chars != 0)
        {
          if (frameOffset_ < sizeof(RxFrame))
          {
            ROS_DEBUG("[%s] Stored chars from the device: %d", name_.c_str(), frameOffset_);
          }
          else
          {
            uint32_t remainder_bytes = frameOffset_ % sizeof(RxFrame);
            ROS_DEBUG("[%s] Whole frame received, remaining bytes: %u", name_.c_str(), remainder_bytes);
            frame = placeholder_;
            if (remainder_bytes != 0)
            {
              memmove((char*)&placeholder_.bytes, (char*)&placeholder_.bytes + sizeof(RxFrame) - remainder_bytes,
                      remainder_bytes);
              memset((char*)&placeholder_.bytes + remainder_bytes, 0, sizeof(RxFrame) - remainder_bytes);
            }
            frameOffset_ = remainder_bytes;
            read_finished = true;
            frameReceivedCounter_++;
          }
          // // reset the timeout counter
          // timeoutCounter_ = 0;
        }
        t_previous = t_current;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_current);
        time_delta += diffSec(t_previous, t_current);
      }
      if (time_delta > readTimeout_)
      {
        increaseAndCheckTimeoutCounter();
        return false;
      }
    }
    catch (std::exception& e)
    {
      ROS_ERROR("[%s] Error while reading a packet, %s", name_.c_str(), e.what());
      errorState_ = ErrorState::PACKET_READING_ERROR;
      return false;
    }
    /* Check if the frame is still aligned, otherwise exit */
    if (frame.header != frameHeader)
    {
      frameSync_ = false;
      /* keep some statistics */
      if (++frameSyncErrorCounter_ >= maxFrameSyncErrorCounts_)
      {
        ROS_WARN("[%s] Reached max syncing errors. Disconnecting sensor.", name_.c_str());
        errorState_ = ErrorState::SYNC_ERROR;
        connectionState_ = ConnectionState::DISCONNECTED;
      }
      return false;
    }
    // Read and check CRC 16-bit
    try
    {
      uint16_t crc_received = frame.crc16_ccitt;
      uint16_t crc_computed = calcCrc16X25(frame.data.bytes, sizeof(frame.data));
      if (crc_received != crc_computed)
      {
        frameCrcErrorCounter_++;
        ROS_WARN("[%s] CRC missmatch received: 0x%04x calculated: 0x%04x", name_.c_str(), crc_received, crc_computed);
        return false;
      }
    }
    catch (std::exception& e)
    {
      ROS_ERROR("[%s] Error while reading a packet, %s", name_.c_str(), e.what());
      errorState_ = ErrorState::PACKET_READING_ERROR;
      return false;
    }

    if (frame.data.status.app_took_too_long)
    {
      ROS_WARN("[%s] Warning force sensor is skipping measurements, Increase sinc length", name_.c_str());
    }
    if (frame.data.status.overrange)
    {
      ROS_WARN("[%s] Warning measuring range exceeded", name_.c_str());
    }
    if (frame.data.status.invalid_measurements)
    {
      ROS_ERROR("[%s] Warning force torque measurements are invalid, Permanent damage may occur", name_.c_str());
    }
    if (frame.data.status.raw_measurements)
    {
      ROS_WARN_THROTTLE(1.0, "[%s] Warning raw force torque measurements from gages", name_.c_str());
    }
    frameSuccessCounter_++;
    return true;
  }
  // This point should not be reached
  return false;
}

bool RokubiminiSerialImpl::isConnected() const
{
  return (connectionState_ == ConnectionState::CONNECTED);
}

bool RokubiminiSerialImpl::isConnecting() const
{
  return (connectionState_ == ConnectionState::ISCONNECTING);
}

bool RokubiminiSerialImpl::hasError() const
{
  return (errorState_ != ErrorState::NO_ERROR);
}

bool RokubiminiSerialImpl::isInConfigMode() const
{
  return (modeState_ == ModeState::CONFIG_MODE);
}

ConnectionState RokubiminiSerialImpl::getConnectionState() const
{
  return connectionState_;
}

ErrorState RokubiminiSerialImpl::getErrorState() const
{
  return errorState_;
}

std::string RokubiminiSerialImpl::getErrorString() const
{
  switch (errorState_)
  {
    case ErrorState::NO_ERROR:
      return std::string{ "No Error" };

    case ErrorState::OFFSET_ERROR:
      return std::string{ "Offset Error" };

    case ErrorState::CALIBRATION_ERROR:
      return std::string{ "Calibration Error" };

    case ErrorState::PACKET_READING_ERROR:
      return std::string{ "Packet Reading Error" };

    case ErrorState::SYNC_ERROR:
      return std::string{ "Sync Error" };

    default:
      return std::string{ "Undefined" };
  }

  return std::string{ "No Error" };
}

bool RokubiminiSerialImpl::openSerialPort(bool keepOpening)
{
  // Block, continuously trying to open and initialize the serial-port, until signaled to stop or the serial port has
  // been opened
  bool open_port = false;
  unsigned int counter_open_serial_port = 0;
  do
  {
    open_port = initSerialPort(port_);
  } while (isRunning_ && keepOpening && !open_port && counter_open_serial_port++ <= maxCountOpenSerialPort_);
  if (!isRunning_)
  {
    // opening process was cancelled from somewhere else
    ROS_ERROR("[%s] Sensor is disconnected.", name_.c_str());
    connectionState_ = ConnectionState::DISCONNECTED;
    return false;
  }
  return true;
}
bool RokubiminiSerialImpl::initSensorCommunication(bool keepOpening)
{
  ROS_DEBUG("[%s] Initializing serial-port at: %s", name_.c_str(), port_.c_str());
  bool success;

  /**
   * @brief Step 1. Open port in every possible baud rate and send hardware reset.
   *
   */
  for (const auto& baud_rate_option : CODE_TO_BAUD_RATE_MAP)
  {
    baudRate_ = baud_rate_option.second.baudRateMask;
    success = openSerialPort(keepOpening);
    if (!success)
    {
      ROS_ERROR("[%s] Could not open serial port %s with baud rate %u", name_.c_str(), port_.c_str(),
                baud_rate_option.second.baudRateInt);
      return false;
    }
    ROS_DEBUG("[%s] Successful initialization of first communication with baud rate %u", name_.c_str(),
              baud_rate_option.second.baudRateInt);
    setHardwareReset();
    closeSerialPort();
  }

  /**
   * @brief Step 2. Open port with default baud rate and parse communication setup.
   *
   */

  baudRate_ = CODE_TO_BAUD_RATE_MAP.find(DEFAULT_BAUD_RATE_OPTION)->second.baudRateMask;
  success = openSerialPort(keepOpening);
  if (!success)
  {
    ROS_ERROR("[%s] Could not open serial port %s with baud rate %u", name_.c_str(), port_.c_str(),
              CODE_TO_BAUD_RATE_MAP.find(DEFAULT_BAUD_RATE_OPTION)->second.baudRateInt);
    return false;
  }

  ROS_DEBUG("[%s] Successful initialization of first communication with default baud rate.", name_.c_str());
  RokubiminiSerialResponseRegexCommSetup comm_setup;
  success = parseRegexWaitTimeout(comm_setup, 2);
  if (!success)
  {
    ROS_ERROR("[%s] Failed to parse successfully the communication setup", name_.c_str());
    return false;
  }
  /**
   * @brief Step 3. Apply the following logic:
   * - If the baud rate parsed is the maximum available, re-open the port with max baud rate and continue.
   * - If the baud rate parsed is the default one, since the port has been opened already with the default baud rate,
   * set the max baud rate, re-open the port with max baud rate and continue.
   * - If the baud rate parsed is neither the default nor the maximum, close the serial port (opened with default baud
   * rate), open the port with actual baud rate, set the max baud rate, re-open the port with max baud rate and
   * continue.
   */
  if (comm_setup.getBaudRate() != MAX_BAUD_RATE_OPTION)
  {
    if (comm_setup.getBaudRate() != DEFAULT_BAUD_RATE_OPTION)
    {
      closeSerialPort();
      baudRate_ = CODE_TO_BAUD_RATE_MAP.find((comm_setup.getBaudRate()))->second.baudRateMask;
      success = openSerialPort(keepOpening);
      if (!success)
      {
        ROS_ERROR("[%s] Could not open serial port %s with baud rate %u", name_.c_str(), port_.c_str(),
                  CODE_TO_BAUD_RATE_MAP.find((comm_setup.getBaudRate()))->second.baudRateInt);
        return false;
      }
      ROS_DEBUG("[%s] Successful initialization of second communication with baud rate %u", name_.c_str(),
                CODE_TO_BAUD_RATE_MAP.find((comm_setup.getBaudRate()))->second.baudRateInt);
    }
    // Sleep for 2.0 seconds for the sensor to boot and start sending data.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ROS_DEBUG("[%s] Setting max baud rate to device", name_.c_str());
    // set the maximum baud rate
    uint8_t max_baud_rate = MAX_BAUD_RATE_OPTION;
    comm_setup.setBaudRate(max_baud_rate);
    if (!setInitMode())
    {
      ROS_ERROR("[%s] Device could not switch to init mode", name_.c_str());
      return false;
    }
    // set the sensor in configuration mode.
    if (!setConfigMode())
    {
      ROS_ERROR("[%s] Device could not switch to config mode", name_.c_str());
      return false;
    }
    // leave the rest of the fields (temp. comp., calibration active, data format) as we found them
    configuration::SensorConfiguration sensor_config;
    sensor_config.setTemperatureCompensationActive(comm_setup.getTempComp());
    sensor_config.setCalibrationMatrixActive(comm_setup.getCalibration());
    uint8_t data_format = 0;  // this is required for the driver to operate in binary mode
    // set the communication setup with the new max baud rate
    if (!setCommunicationSetup(sensor_config, data_format, comm_setup.getBaudRate()))
    {
      ROS_ERROR("[%s] Could not set the communication setup", name_.c_str());
      return false;
    }
    // save the configuration
    if (!saveConfigParameter())
    {
      ROS_ERROR("[%s] Could not save the configuration to the device", name_.c_str());
      return false;
    }
    // set hardware reset to device
    if (!setHardwareReset())
    {
      ROS_ERROR("[%s] Device could not switch to run mode", name_.c_str());
      return false;
    }
  }
  closeSerialPort();
  ROS_DEBUG("[%s] Re-initializing serial-port at: %s with maximum baud rate", name_.c_str(), port_.c_str());

  baudRate_ = CODE_TO_BAUD_RATE_MAP.find(MAX_BAUD_RATE_OPTION)->second.baudRateMask;
  success = openSerialPort(keepOpening);
  if (!success)
  {
    ROS_ERROR("[%s] Could not open serial port %s with baud rate %u", name_.c_str(), port_.c_str(),
              CODE_TO_BAUD_RATE_MAP.find(MAX_BAUD_RATE_OPTION)->second.baudRateInt);
    return false;
  }
  // Finally if the whole process succeeded, change the connection state to CONNECTED.
  connectionState_ = ConnectionState::CONNECTED;
  return true;
}

bool RokubiminiSerialImpl::parseRegexWaitTimeout(RokubiminiSerialResponseRegex& reg, const double& timeout)
{
  std::string str, tmp_str;
  timespec t_start, t_current;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);
  t_current = t_start;
  while (diffSec(t_start, t_current) < timeout)  // run the loop for timeout seconds.
  {
    readSerialNoWait(1, tmp_str);
    str.append(tmp_str);
    if (reg.matchInString(str))
    {
      ROS_DEBUG("[%s] Found exact match: %s, time: %f", name_.c_str(), str.c_str(), diffSec(t_start, t_current));
      return true;
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_current);
  }
  return false;
}
bool RokubiminiSerialImpl::readSerialNoWait(const uint32_t& numChars, std::string& str)
{
  char buffer[numChars];
  uint32_t stored_chars;
  stored_chars = usbStreamIn_.readsome(buffer, sizeof(buffer));
  str.assign(buffer, stored_chars);
  // return whether there were any chars to read
  return (stored_chars != 0);
}

bool RokubiminiSerialImpl::readSerialWaitTimeout(const uint32_t& numChars, std::string& str, const double& timeout)
{
  char buffer[numChars];
  uint32_t read_offset = 0, stored_chars;
  timespec t_start, t_current;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);
  t_current = t_start;
  while (diffSec(t_start, t_current) < timeout)  // run the loop for timeout seconds.
  {
    /* run the next readsome() only if there are available bytes in the input buffer.*/
    if (usbStreamIn_.rdbuf()->in_avail() > 0)
    {
      stored_chars = usbStreamIn_.readsome((char*)&buffer[read_offset], sizeof(buffer) - read_offset);
      read_offset += stored_chars;
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_current);
  }
  str.assign(buffer, read_offset);
  // return whether there were any chars to read
  return (read_offset != 0);
}
bool RokubiminiSerialImpl::parseCommunicationMsgs(const double& timeout)
{
  RokubiminiSerialResponseRegexProductName product_name_regex;
  bool success = parseRegexWaitTimeout(product_name_regex);
  if (!success)
  {
    ROS_ERROR("[%s] Failed to parse Product Name of the serial device", name_.c_str());
    return false;
  }
  productName_ = product_name_regex.getProductName();
  RokubiminiSerialResponseRegexSerialNumber serial_number_regex;
  success &= parseRegexWaitTimeout(serial_number_regex);
  if (!success)
  {
    ROS_ERROR("[%s] Failed to parse Serial Number of the serial device", name_.c_str());
    return false;
  }
  serialNumber_ = serial_number_regex.getSerialNumber();
  return true;
}

bool RokubiminiSerialImpl::initSerialPort(const std::string& port)
{
  // int flags;
  termios newtio;
  struct serial_struct ser_info;

  // make the fstreams unbuffered
  usbStreamIn_.rdbuf()->pubsetbuf(nullptr, 0);
  usbStreamOut_.rdbuf()->pubsetbuf(nullptr, 0);

  // Open the Serial Port
  usbFileDescriptor_ = open(port.c_str(), O_RDWR | O_NOCTTY);
  if (usbFileDescriptor_ < 0)
  {
    ROS_ERROR_THROTTLE(1.0, "[%s] Failed to open serial-port: '%s'", name_.c_str(), port.c_str());
    return false;
  }

  // Get the Serial Descriptor Flags
  if (tcgetattr(usbFileDescriptor_, &newtio) < 0)
  {
    ROS_ERROR("[%s] Failed to get connection attributes.", name_.c_str());
    return false;
  }

  // Set the Serial Speed
  cfsetispeed(&newtio, baudRate_);
  cfsetospeed(&newtio, baudRate_);

  cfmakeraw(&newtio);

  // Set the Serial Connection Attributes (SCA)
  if (tcsetattr(usbFileDescriptor_, TCSAFLUSH, &newtio) < 0)
  {
    ROS_ERROR("[%s] Failed to set connection attributes.", name_.c_str());
    return false;
  }

  // Flush the input and output streams
  if (tcflush(usbFileDescriptor_, TCIOFLUSH) < 0)
  {
    ROS_ERROR("[%s] Failed to flush the input and output streams.", name_.c_str());
    return false;
  }

  // Enable linux FTDI low latency mode
  ioctl(usbFileDescriptor_, TIOCGSERIAL, &ser_info);
  ser_info.flags |= ASYNC_LOW_LATENCY;
  ioctl(usbFileDescriptor_, TIOCSSERIAL, &ser_info);

  // Get the Serial Descriptor Flags
  if (fcntl(usbFileDescriptor_, F_GETFL) < 0)
  {
    ROS_ERROR("[%s] Failed to set the descriptor flags.", name_.c_str());
    return false;
  }

  // create a std stream to read and write from/to device
  usbStreamIn_.open(port.c_str(), std::ifstream::in);
  usbStreamOut_.open(port.c_str(), std::ofstream::out);
  if (((usbStreamIn_.fail() | usbStreamOut_.fail()) != 0))
  {
    ROS_ERROR("[%s] Failed to open file streams.", name_.c_str());
    return false;
  }
  return true;
}

inline uint16_t RokubiminiSerialImpl::calcCrc16X25(uint8_t* data, int len)
{
  uint16_t crc = 0xFFFF;
  while (len--)
    crc = crcCcittUpdate(crc, *data++);
  return ~crc;
}

uint16_t RokubiminiSerialImpl::crcCcittUpdate(uint16_t crc, uint8_t data)
{
#define lo8(x) ((x)&0xFF)
#define hi8(x) (((x) >> 8) & 0xFF)
  data ^= lo8(crc);
  data ^= data << 4;

  return ((((uint16_t)data << 8) | hi8(crc)) ^ (uint8_t)(data >> 4) ^ ((uint16_t)data << 3));
}

void RokubiminiSerialImpl::connectionWorker()
{
  initSensorCommunication(true);
}

void RokubiminiSerialImpl::pollingWorker()
{
  // Configure the scheduling policy for the pollin gthread
  int priority = 99;
  sched_param params;
  params.sched_priority = priority;
  if (sched_setscheduler(getpid(), SCHED_FIFO, &params) != 0)
  {
    ROS_WARN("[%s] Failed to set thread priority to %d: %s.", name_.c_str(), priority, std::strerror(errno));
  }

  // Wait for connection to be established before starting to poll
  usleep(100000);
  if (!isConnected())
  {
    while (!isConnected() && isRunning_)
    {
      usleep(1000);
      ROS_WARN_THROTTLE(4.0, "[%s] Polling thread waiting for connection. ", name_.c_str());
    }
  }
  uint32_t time_stamp_previous, time_stamp = 0;
  double time_stamp_diff;

  // Variables for timing
  timespec t_current, t_cycle = { 0 /* initialize t_cycle to avoid may-be-uninitialized errors */ }, t_loop;
  bool run_processing = true;
  unsigned long loop_counter = 0;
  pollingSyncErrorCounter_ = 0;
  double int_part;
  if (!runsAsync())
  {
    // create timespec for the cycle time by breaking the time step into integral and fractional part with modf()
    t_cycle.tv_nsec = NSEC_PER_SEC * modf(pollingThreadTimeStep_, &int_part);
    t_cycle.tv_sec = static_cast<uint64_t>(int_part);
    // make the read timeout depend on the input time step padded with a margin
    readTimeout_ = FTDI_DRIVER_LATENCY + (1 + TIMEOUT_MARGIN) * pollingThreadTimeStep_;
  }

  // Start sensor polling
  while (isRunning_ && modeState_ == ModeState::RUN_MODE)
  {
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_current);
    // Poll the serial device, skipping processing in current iteration if no valid data has been received
    RxFrame frame;

    std::unique_lock<std::recursive_mutex> lock(serialMutex_);
    (this->readDevice(frame)) ? (run_processing = true) : (run_processing = false);
    lock.unlock();
    // TODO: check if PID of sent data and PID configured match
    // We have to check the connection again because readDevice() may have signalled to disconnect
    // if too many communication errors occurred
    if (connectionState_ == ConnectionState::CONNECTED && run_processing)
    {
      /* Set the time-stamp */
      time_stamp_previous = time_stamp;
      time_stamp = frame.data.timestamp;
      time_stamp_diff = static_cast<double>(time_stamp - time_stamp_previous) * 1e-6;
      if (time_stamp_previous > time_stamp)
      {
        ROS_WARN("[%s] Time stamp difference is negative: Time counter has wrapped: current time stamp=%u, previous "
                 "time stamp=%u and  time "
                 "stamp diff=%f",
                 name_.c_str(), time_stamp, time_stamp_previous, time_stamp_diff);
      }
      // check for polling errors
      if ((fabs(time_stamp_diff - pollingThreadTimeStep_) >
           FTDI_DRIVER_LATENCY + TIMEOUT_MARGIN * pollingThreadTimeStep_) &&
          frameSuccessCounter_ > 1 && !runsAsync())
      {
        pollingSyncErrorCounter_++;
        ROS_WARN("[%s] Polling is out of sync! time step=%f and time stamp diff=%f", name_.c_str(),
                 pollingThreadTimeStep_, time_stamp_diff);
      }
      if (runsAsync())
      {
        // change the t_cycle so that the loop time can adapt to the Hz of the sensor
        readTimeout_ = (time_stamp_diff < MAXIMUM_ACCEPTABLE_TIMEOUT) ?
                           FTDI_DRIVER_LATENCY + (1 + TIMEOUT_MARGIN) * time_stamp_diff :
                           MAXIMUM_ACCEPTABLE_TIMEOUT;
        // make the time stamp difference zero in the first time
        if (frameSuccessCounter_ == 1)
        {
          time_stamp_diff = 0;
        }
        t_cycle.tv_nsec = NSEC_PER_SEC * modf(time_stamp_diff, &int_part);
        t_cycle.tv_sec = static_cast<uint64_t>(int_part);
      }
      // Store the final measurement into the primary buffer
      {
        std::lock_guard<std::mutex> lock(readingMutex_);
        // check if it's the first frame received and set the timestamp offset
        if (frameTimestampOffset_.sec == 0 && frameTimestampOffset_.nsec == 0)
        {
          frameTimestampOffset_ = ros::Time::now();
          frameTimestampOffsetIsValid_ = true;
        }
        else
        {
          frameTimestampSecs_ += time_stamp_diff;
        }
        frame_ = frame;
        dataReady_ = true;
      }
      if (runsAsync())
      {
        newFrameIsAvailable_.notify_one();
      }
    }
    t_loop = timespecAdd(t_current, t_cycle);
    // Sleep to implement cyclic polling
    clock_nanosleep(CLOCK_MONOTONIC_RAW, TIMER_ABSTIME, &t_loop, nullptr);
    loop_counter++;
  }

  // Show error statistics if errors have been detected
  if ((pollingSyncErrorCounter_ > 0) || (frameSyncErrorCounter_ > 0) || (frameCrcErrorCounter_ > 0) ||
      (timeoutCounter_ > 0))
  {
    ROS_WARN("[%s] Sensor polling thread error report:", name_.c_str());
    ROS_WARN("[%s]   Total iterations:    %lu", name_.c_str(), loop_counter);
    ROS_WARN("[%s]   Frames received:     %lu, (%f%%)", name_.c_str(), frameReceivedCounter_,
             100.0 * ((double)frameReceivedCounter_ / (double)loop_counter));
    ROS_WARN("[%s]   Correct frames:      %lu, (%f%%)", name_.c_str(), frameSuccessCounter_,
             100.0 * ((double)frameSuccessCounter_ / (double)frameReceivedCounter_));
    ROS_WARN("[%s]   Polling sync errors: %lu, (%f%%)", name_.c_str(), pollingSyncErrorCounter_,
             100.0 * ((double)pollingSyncErrorCounter_ / (double)loop_counter));
    ROS_WARN("[%s]   Frame CRC errors:    %lu, (%f%%)", name_.c_str(), frameCrcErrorCounter_,
             100.0 * ((double)frameCrcErrorCounter_ / (double)frameReceivedCounter_));
    ROS_WARN("[%s]   Frame sync errors:   %d", name_.c_str(), frameSyncErrorCounter_);
    ROS_WARN("[%s]   Timeout errors:      %d", name_.c_str(), timeoutCounter_);
  }
}

void RokubiminiSerialImpl::getReading(rokubimini::Reading& reading)
{
  if (isConnected())
  {
    {
      std::unique_lock<std::mutex> lock(readingMutex_);
      if (runsAsync())
      {
        newFrameIsAvailable_.wait(lock, [&] { return dataReady_; });
      }
      if (isRunning_)
      {
        dataReady_ = false;
        const auto& stamp = frameTimestampOffset_ + ros::Duration(frameTimestampSecs_);
        serialImplReading_.getWrench().header.stamp = stamp;
        serialImplReading_.getWrench().header.frame_id = name_ + "_wrench";
        serialImplReading_.getWrench().wrench.force.x = frame_.data.forces[0];
        serialImplReading_.getWrench().wrench.force.y = frame_.data.forces[1];
        serialImplReading_.getWrench().wrench.force.z = frame_.data.forces[2];
        serialImplReading_.getWrench().wrench.torque.x = frame_.data.forces[3];
        serialImplReading_.getWrench().wrench.torque.y = frame_.data.forces[4];
        serialImplReading_.getWrench().wrench.torque.z = frame_.data.forces[5];

        serialImplReading_.getTemperature().header.stamp = stamp;
        serialImplReading_.getTemperature().header.frame_id = name_ + "_temp";
        serialImplReading_.getTemperature().temperature = frame_.data.temperature;
        serialImplReading_.getTemperature().variance = 0;  // unknown variance
        reading = serialImplReading_;
      }
    }
  }
}

bool RokubiminiSerialImpl::setConfigMode()
{
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  auto config_command = RokubiminiSerialCommandConfig();
  std::string str;
  bool success = true;
  if (!config_command.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the config command", name_.c_str());
    return false;
  }
  success &= sendCommand(str);
  if (!success)
  {
    lock.unlock();
    return false;
  }
  else
  {
    modeState_ = ModeState::CONFIG_MODE;
    lock.unlock();
    // required delay for the sensor to load all the defaults
    std::this_thread::sleep_for(std::chrono::microseconds(1500000));
    if (runInThreadedMode_ && pollingThread_.joinable())
    {
      pollingThread_.join();
    }
    // Reset error counters and error state
    frameReceivedCounter_ = 0;
    frameSuccessCounter_ = 0;
    frameCrcErrorCounter_ = 0;
    frameSyncErrorCounter_ = 0;
    timeoutCounter_ = 0;
    errorState_ = ErrorState::NO_ERROR;
    return true;
  }
}

bool RokubiminiSerialImpl::setRunMode()
{
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  auto run_command = RokubiminiSerialCommandRun();
  std::string str;
  if (!run_command.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the run command", name_.c_str());
    return false;
  }
  bool success = sendCommand(str);
  lock.unlock();
  if (!success)
  {
    return false;
  }

  modeState_ = ModeState::RUN_MODE;
  // Start a new polling thread.
  return startPollingThread();
}

bool RokubiminiSerialImpl::setHardwareReset()
{
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  auto hard_reset_command = RokubiminiSerialCommandHardReset();
  std::string str;
  if (!hard_reset_command.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the hardware reset command", name_.c_str());
    return false;
  }
  bool success = writeSerial(str);
  lock.unlock();
  return success;
}

bool RokubiminiSerialImpl::setInitMode()
{
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  auto soft_reset_command = RokubiminiSerialCommandSoftReset();
  std::string str;
  if (!soft_reset_command.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the software reset command", name_.c_str());
    return false;
  }
  bool success = sendCommand(str);
  if (!success)
  {
    lock.unlock();
    return false;
  }
  else
  {
    modeState_ = ModeState::CONFIG_MODE;
    lock.unlock();

    std::this_thread::sleep_for(std::chrono::microseconds(500000));
    if (runInThreadedMode_ && pollingThread_.joinable())
    {
      pollingThread_.join();
    }
    // Reset error counters and error state
    frameReceivedCounter_ = 0;
    frameSuccessCounter_ = 0;
    frameCrcErrorCounter_ = 0;
    frameSyncErrorCounter_ = 0;
    errorState_ = ErrorState::NO_ERROR;
    return true;
  }
}

bool RokubiminiSerialImpl::setForceTorqueFilter(const configuration::ForceTorqueFilter& filter)
{
  if (!isInConfigMode())
  {
    return false;
  }

  ROS_DEBUG("[%s] Setting force/torque filter", name_.c_str());
  ROS_DEBUG("[%s] \tsize: %u", name_.c_str(), filter.getSincFilterSize());
  ROS_DEBUG("[%s] \tchop: %u", name_.c_str(), filter.getChopEnable());
  ROS_DEBUG("[%s] \tfast: %u", name_.c_str(), filter.getFastEnable());
  ROS_DEBUG("[%s] \tskip: %u", name_.c_str(), filter.getSkipEnable());

  auto filter_command = RokubiminiSerialCommandFilter(filter);
  std::string str;
  if (!filter_command.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the filter command", name_.c_str());
    return false;
  }
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  bool success = sendCommand(str);
  lock.unlock();
  return success;
}

bool RokubiminiSerialImpl::setForceTorqueOffset(const Eigen::Matrix<double, 6, 1>& forceTorqueOffset)
{
  if (!isInConfigMode())
  {
    return false;
  }

  ROS_DEBUG_STREAM("[" << name_.c_str() << "] Setting Force/Torque offset: " << forceTorqueOffset << std::endl);
  auto offset_command = RokubiminiSerialCommandOffset(forceTorqueOffset);
  std::string str;
  if (!offset_command.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the force torque offset command", name_.c_str());
    return false;
  }
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  bool success = sendCommand(str);
  lock.unlock();
  return success;
}

bool RokubiminiSerialImpl::setSensorCalibration(const calibration::SensorCalibration& sensorCalibration)
{
  if (!isInConfigMode())
  {
    return false;
  }
  char buffer[100];
  bool success = true;
  for (uint32_t row = 0; row < 6; row++)
  {
    auto sensor_calibration_row_command = RokubiminiSerialCommandSensorCalibrationRow(
        sensorCalibration.getCalibrationMatrix()(row, 0), sensorCalibration.getCalibrationMatrix()(row, 1),
        sensorCalibration.getCalibrationMatrix()(row, 2), sensorCalibration.getCalibrationMatrix()(row, 3),
        sensorCalibration.getCalibrationMatrix()(row, 4), sensorCalibration.getCalibrationMatrix()(row, 5), row);
    std::string str;
    if (!sensor_calibration_row_command.formatCommand(str))
    {
      ROS_ERROR("[%s] Could not format the calibration matrix command", name_.c_str());
      return false;
    }
    ROS_DEBUG_STREAM("[" << name_.c_str() << "] Calibration matrix line is: " << str << ". Size is " << str.size());
    std::unique_lock<std::recursive_mutex> lock(serialMutex_);
    success &= sendCommand(str);
    lock.unlock();
    memset(buffer, 0, sizeof(buffer));
    // Sleep for 10.0 milliseconds before sending the next chunk of data.
    std::this_thread::sleep_for(std::chrono::microseconds(10000));
  }
  return success;
}

bool RokubiminiSerialImpl::setSensorConfiguration(const configuration::SensorConfiguration& sensorConfiguration)
{
  if (!isInConfigMode())
  {
    return false;
  }
  ROS_DEBUG("[%s] Setting sensor configuration", name_.c_str());
  uint8_t baud_rate = MAX_BAUD_RATE_OPTION;
  uint8_t data_format = 0;
  return setCommunicationSetup(sensorConfiguration, data_format, baud_rate);
}
bool RokubiminiSerialImpl::setCommunicationSetup(const configuration::SensorConfiguration& sensorConfiguration,
                                                 const uint8_t& dataFormat, const uint8_t& baudRate)
{
  ROS_DEBUG("[%s] Setting communication setup with baud rate: %u, data format: %u, temp comp: %u and calibration: %u",
            name_.c_str(), baudRate, dataFormat, sensorConfiguration.getTemperatureCompensationActive(),
            sensorConfiguration.getCalibrationMatrixActive());
  auto comm_setup = RokubiminiSerialCommandCommSetup(sensorConfiguration, dataFormat, baudRate);
  std::string str;
  if (!comm_setup.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the communication setup command", name_.c_str());
    return false;
  }
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  bool success = sendCommand(str);
  lock.unlock();
  return success;
}

bool RokubiminiSerialImpl::saveConfigParameter()
{
  if (!isInConfigMode())
  {
    return false;
  }
  auto save_config = RokubiminiSerialCommandSave();
  std::string str;
  if (!save_config.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the save config command", name_.c_str());
    return false;
  }
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  bool success = sendCommand(str);
  lock.unlock();
  return success;
}
bool RokubiminiSerialImpl::loadConfig()
{
  if (!isInConfigMode())
  {
    return false;
  }
  auto load_config = RokubiminiSerialCommandLoad();
  std::string str;
  if (!load_config.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the load config command", name_.c_str());
    return false;
  }
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  bool success = sendCommand(str);
  lock.unlock();
  return success;
}
bool RokubiminiSerialImpl::printUserConfig()
{
  if (!isInConfigMode())
  {
    return false;
  }
  auto print_config = RokubiminiSerialCommandPrint();
  std::string str;
  if (!print_config.formatCommand(str))
  {
    ROS_ERROR("[%s] Could not format the print config command", name_.c_str());
    return false;
  }
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  bool success = sendCommand(str);
  lock.unlock();
  if (!success)
  {
    return false;
  }
  uint8_t c;
  uint64_t timespec_diff_ns;
  timespec tnow, t_loop;
  clock_gettime(CLOCK_MONOTONIC_RAW, &tnow);
  t_loop = tnow;
  lock.lock();
  ROS_INFO("[%s] Printing user configuration:", name_.c_str());
  do
  {
    /* run the next readsome() only if there are available bytes in the input buffer.*/
    if (usbStreamIn_.rdbuf()->in_avail() > 0)
    {
      /* read bytes 1 by 1 and print them.*/
      usbStreamIn_.readsome((char*)&c, 1);
      printf("%c", c);
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_loop);
    timespec_diff_ns = (t_loop.tv_sec - tnow.tv_sec) * 1e9 + (t_loop.tv_nsec - tnow.tv_nsec);
  } while (timespec_diff_ns < 1e9);  // run the loop for 1sec.
  lock.unlock();
  return true;
}

bool RokubiminiSerialImpl::sendCommand(const std::string& command)
{
  bool success = true;
  success &= clearReadBuffer();
  success &= writeSerial(command);
  success &= parseAcknowledgement(command.front());  // the first character represents the type of command
  return success;
}

bool RokubiminiSerialImpl::clearReadBuffer()
{
  ROS_DEBUG("[%s] Clearing read buffer", name_.c_str());
  // Flush the input streams
  if (tcflush(usbFileDescriptor_, TCIFLUSH) < 0)
  {
    ROS_ERROR("[%s] Failed to flush the input read buffer.", name_.c_str());
    return false;
  }
  return true;
}

bool RokubiminiSerialImpl::parseAcknowledgement(const char& command_code, const double& timeout)
{
  RokubiminiSerialResponseRegexAck ack;
  bool success = parseRegexWaitTimeout(ack, timeout);
  if (!success)
  {
    ROS_ERROR("[%s] Command not found in ACK", name_.c_str());
    return false;
  }
  if (command_code != ack.getCommand())
  {
    ROS_ERROR("[%s] Didn't find the correct command in ACK", name_.c_str());
    return false;
  }
  if (ack.getReturnCode() != 0)
  {
    ROS_ERROR("[%s] Device responded with error code: %u", name_.c_str(), ack.getReturnCode());
    return false;
  }
  return true;
}
bool RokubiminiSerialImpl::writeSerial(const std::string& str)
{
  try
  {
    // check that the string size doesn't exceed the serial input buffer's size (64) of the device.
    if (str.size() > 64)
    {
      ROS_WARN("[%s] String's length exceeds permittable limit (64)", name_.c_str());
      return false;
    }
    ROS_DEBUG("[%s] Number of chars: %zu", name_.c_str(), str.size());
    ROS_DEBUG("[%s] String chars: %s", name_.c_str(), str.c_str());
    if (usbStreamIn_.is_open() && usbStreamOut_.is_open())
    {
      usbStreamIn_.sync();
      char cstr[str.size() + 1];
      strcpy(cstr, str.c_str());
      for (uint8_t i = 0; i < str.size(); i++)
      {
        usbStreamOut_.put(cstr[i]);
        std::this_thread::sleep_for(std::chrono::microseconds(5000));
        usbStreamOut_.flush();
      }
      if ((usbStreamOut_.fail() | usbStreamIn_.fail()) != 0)
      {
        ROS_WARN("[%s] Serial Write or Read failed", name_.c_str());
        return false;
      }
    }
    else
    {
      ROS_WARN("[%s] Streams are not yet open.", name_.c_str());
      return false;
    }
  }
  catch (const std::exception& e)
  {
    ROS_ERROR("[%s] %s", name_.c_str(), e.what());
    return false;
  }
  return true;
}

bool RokubiminiSerialImpl::firmwareUpdate(const std::string& filePath)
{
  pid_t pid;
  int status;
  ROS_INFO("Flashing firmware...");
  std::unique_lock<std::recursive_mutex> lock(serialMutex_);
  setHardwareReset();
  pid = fork();

  if (pid == -1)
  {
    // pid == -1 means error occured
    ROS_ERROR("Could not fork, error occured");
    lock.unlock();
    return false;
  }
  else if (pid == 0)
  {
    ROS_DEBUG("Child process, pid = %u\n", getpid());
    // pid == 0 means child process created
    char path_argument[filePath.size() + 15];
    int ret = sprintf(path_argument, "-Uflash:w:%s:i", filePath.c_str());
    if (ret < 0)
    {
      ROS_ERROR("[%s] sprintf failed to write data", name_.c_str());
      lock.unlock();
      return false;
    }
    char port_argument[port_.size() + 2];
    ret = sprintf(port_argument, "-P%s", port_.c_str());
    if (ret < 0)
    {
      ROS_ERROR("[%s] sprintf failed to write data", name_.c_str());
      lock.unlock();
      return false;
    }
    // the argv list first argument should point to
    // filename associated with file being executed
    // the array pointer must be terminated by a NULL
    // pointer
    const char* argv_list[9] = { "avrdude",  "-v", "-patmega328p", "-carduino", port_argument,
                                 "-b230400", "-D", path_argument,  nullptr };
    ROS_DEBUG("Complete command for avrdude:");
    for (std::size_t i = 0; i < sizeof(argv_list); i++)
    {
      ROS_DEBUG("%s", argv_list[i]);
    }
    ROS_DEBUG("\n");
    // the execv() only returns if error occurred (then the return value is -1)
    ret = execvp(argv_list[0], const_cast<char* const*>(argv_list));
    if (ret < 0)
    {
      ROS_ERROR("[%s] Execution of avrdude failed: %s", name_.c_str(), strerror(errno));
      kill(getpid(), SIGKILL);
    }
    exit(0);
  }
  else
  {
    // a positive number is returned which is the pid of child process
    // the parent process calls waitpid() on the child.
    // waitpid() system call suspends execution of
    // calling process until a child specified by pid
    // argument has changed state
    // see wait() man page for all the flags or options
    // used here
    if (waitpid(pid, &status, WUNTRACED) > 0)
    {
      if (WIFEXITED(status) && !WEXITSTATUS(status))
      {
        ROS_INFO("[%s] avrdude returned successfully", name_.c_str());
        // stop polling and prepare to shutdown
        isRunning_ = false;
      }
      else if (WIFEXITED(status) && WEXITSTATUS(status))
      {
        if (WEXITSTATUS(status) == 127)
        {
          // execvp failed
          ROS_ERROR("[%s] execvp failed", name_.c_str());
          lock.unlock();
          return false;
        }
        else
        {
          ROS_ERROR("[%s] program terminated normally,"
                    " but returned a non-zero status",
                    name_.c_str());
          // stop polling and prepare to shutdown
          isRunning_ = false;
          lock.unlock();
          return false;
        }
      }
      else
      {
        ROS_ERROR("[%s] program didn't terminate normally", name_.c_str());
        lock.unlock();
        return false;
      }
    }
    else
    {
      // waitpid() failed
      ROS_ERROR("[%s] waitpid() failed", name_.c_str());
      lock.unlock();
      return false;
    }
  }
  lock.unlock();
  return true;
}
}  // namespace serial
}  // namespace rokubimini
