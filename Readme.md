## Controller Configuration

The system expects specific controller IDs:
- **Controller ID=4**: XYZ linear stages (Y=axis0, X=axis1, Z=axis2)
- **Controller ID=2222**: R rotational stage (R=axis0)

## Installation

### 1. Install Dependencies

```bash
# Install build tools
sudo apt-get update
sudo apt-get install build-essential

# Install MQTT library
sudo apt-get install libmosquitto-dev

# Install MQTT broker (if running locally)
sudo apt-get install mosquitto mosquitto-clients

# Start MQTT broker
sudo systemctl start mosquitto
sudo systemctl enable mosquitto
```

### 2. Prepare ECC100 SDK Files

Ensure you have the following files in your project directory:
```
├── ecc.h                      # ECC100 header file
├── libecc.so                 # ECC100 shared library  
├── ecc_mqtt_streaming.cpp    # Code for broadcasting data and listening commands
├── ecc_tool.cpp              # Code for manual moving and testing
└── README.md                 # This file
```

### 3. Compile
```bash
g++ -std=c++11 -Wall -Wextra -O2 -D__unix__ -Dunix -Wl,-rpath,. -o ecc_tool ecc_tool.cpp -lecc -L. -I.
```

```bash
g++ -std=c++11 -Wall -Wextra -O2 -D__unix__ -Dunix -Wl,-rpath,. -pthread -o ecc_mqtt_streaming ecc_mqtt_streaming.cpp -lecc -lmosquitto -L. -I.
```
**Note**: Ensure `libecc.so` is in the same directory or in your library path.

## Configuration

### ecc_tool - Direct Controller Interface

Single-operation command-line tool for immediate controller operations.

### Features
- List and inspect all connected controllers
- Precise positioning with progress monitoring
- Manual and automatic calibration
- Single-step and continuous movement
- Parameter configuration and saving
- Real-time position monitoring
- Movement stopping functionality

### Basic Syntax
```bash
./ecc_tool <command> [arguments...]
```

### Commands

#### 1. List Controllers and Status
```bash
./ecc_tool list
```
**Output Example:**
```
Controller 0 (ID=4, Handle=1)
Firmware Version: 538313767
  Axis 0: 999730 nm [Linear] (ECS3030) [REF]
    Amplitude: 45000 mV
    Frequency: 1000000 mHz
    Target range: 1000 nm/µ°
    Reference valid: Yes
    Reference position: 0
    EOT Forward: Clear
    EOT Backward: Clear
```

#### 2. Move to Position
```bash
./ecc_tool move <controller_index> <axis> <target_position>
```
**Examples:**
```bash
./ecc_tool move 0 1 5000        # Move controller 0, axis 1 to 5000 nm
./ecc_tool move 1 0 -1200000    # Move controller 1, axis 0 to -1200000 µ°
```

#### 3. Calibrate Axis
```bash
./ecc_tool calibrate <controller_index> <axis>
```
**Example:**
```bash
./ecc_tool calibrate 1 0        # Reset position and establish reference
```

#### 4. Continuous Movement
```bash
./ecc_tool continuous <controller_index> <axis> <direction> [duration_ms]
```
**Examples:**
```bash
./ecc_tool continuous 0 1 forward 2000    # Move forward for 2 seconds
./ecc_tool continuous 1 0 backward 1000   # Move backward for 1 second
```

#### 5. Single Step Movement
```bash
./ecc_tool step <controller_index> <axis> <direction> [num_steps]
```
**Examples:**
```bash
./ecc_tool step 0 1 forward 5     # 5 steps forward
./ecc_tool step 1 0 backward 10   # 10 steps backward
```

#### 6. Monitor Position
```bash
./ecc_tool monitor <controller_index> <axis> [duration_seconds]
```
**Example:**
```bash
./ecc_tool monitor 1 0 30         # Monitor axis for 30 seconds
```

#### 7. Configure Parameters
```bash
./ecc_tool config <controller_index> <axis> [amplitude_mV] [frequency_mHz]
```
**Examples:**
```bash
./ecc_tool config 0 1 30000 1000000    # Set amplitude and frequency
./ecc_tool config 0 1 45000            # Set amplitude only
```

#### 8. Stop Movement
```bash
./ecc_tool stop <controller_index> <axis>
```
**Example:**
```bash
./ecc_tool stop 1 0               # Stop closed-loop control
```

#### 9. Save Configuration
```bash
./ecc_tool save <controller_index>
```
**Example:**
```bash
./ecc_tool save 0                 # Save settings to flash memory
```

### Important Notes
- **Units**: Linear actuators use nanometers (nm), goniometers/rotators use micro-degrees (µ°)
- **Target Range**: Automatically calculated as 10% of movement distance (minimum 1000 units)
- **Moving Status**: `[MOVING]` indicates active closed-loop positioning control
- **Reference**: `[REF]` indicates valid position reference established

---

## ecc_mqtt_streaming
### System Parameters

Edit the following constants in `ecc_mqtt_streaming.cpp`:

```cpp
const int SAMPLE_RATE_HZ = 10000;              // Position sampling rate (10kHz)
const std::string MQTT_BROKER = "localhost";  // MQTT broker address
const int MQTT_PORT = 1883;                   // MQTT broker port
const int XYZ_CONTROLLER_ID = 4;               // XYZ controller ID
const int R_CONTROLLER_ID = 2222;              // R controller ID
```

### MQTT Topics

```cpp
const std::string MQTT_TOPIC_POSITION = "microscope/stage/position";  // Position stream
const std::string MQTT_TOPIC_COMMAND = "microscope/stage/command";    // Movement commands
const std::string MQTT_TOPIC_RESULT = "microscope/stage/result";      // Command results & errors
const std::string MQTT_TOPIC_STATUS = "microscope/stage/status";      // System status
```

### Hardware Mapping

- **XYZ Controller (ID=4)**: Y (Physical Axis 0), X (Physical Axis 1), Z (Physical Axis 2)
- **R Controller (ID=2222)**: R (Physical Axis 0)

### Starting the System

```bash
./ecc_mqtt_streaming
```

**Expected Output:**
```
ECC100 MQTT Streaming Control System Starting...

Sample Rate: 10000 Hz
MQTT Broker: localhost:1883
Position Topic: microscope/stage/position
Command Topic: microscope/stage/command
Result Topic: microscope/stage/result

MQTT connected to broker
Subscribed to: microscope/stage/command
MQTT connected successfully

Found 2 controller(s):
  XYZ Controller (ID=4, Index=0)
    Y (Physical Axis 0) connected and enabled
    X (Physical Axis 1) connected and enabled
    Z (Physical Axis 2) connected and enabled
  R Controller (ID=2222, Index=1)
    R (Physical Axis 0) connected and enabled

Position publisher thread started (10000 Hz)
Command receiver thread started
Error monitor thread started
All threads started. System ready.

Publishing positions to: microscope/stage/position
Listening for commands on: microscope/stage/command
Results and errors on: microscope/stage/result

Press Ctrl+C to stop.
```

### Sending Commands

#### Command Format
Commands are sent to the `microscope/stage/command` topic using the format:
```
<COMMAND>/<AXIS>/<PARAMETER>
```

#### Movement Commands
```bash
# Move X axis to position 5000 nm
mosquitto_pub -h localhost -t "microscope/stage/command" -m "MOVE/X/5000"

# Move Y axis to position -2500 nm  
mosquitto_pub -h localhost -t "microscope/stage/command" -m "MOVE/Y/-2500"

# Move Z axis to position 10000 nm
mosquitto_pub -h localhost -t "microscope/stage/command" -m "MOVE/Z/10000"

# Move R axis to 90 degrees (90000 micro-degrees)
mosquitto_pub -h localhost -t "microscope/stage/command" -m "MOVE/R/90000"
```

#### Stop Commands
```bash
# Stop X axis movement (disable closed-loop control)
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STOP/X"

# Stop all axes (send individual stop commands)
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STOP/X"
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STOP/Y"
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STOP/Z"
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STOP/R"
```

**STOP Command Behavior:**
- **Disables closed-loop movement control** - stops active positioning
- **Maintains output stage** - keeps axis powered and ready
- **Immediate effect** - stops movement instantly
- **Safe operation** - can resume movement with new MOVE commands

#### System Status Command
```bash
# Get detailed system status (equivalent to "ecc_tool list")
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STATUS"
```

**STATUS Command Output:**
The STATUS command generates a comprehensive system report published to the result topic, including:
- MQTT and controller connection status
- Firmware versions and controller IDs
- Current positions for all axes with correct logical names (X, Y, Z, R)
- Performance statistics (actual frequency, missed deadlines)
- Amplitude, frequency, and target range settings
- Movement status, reference status, and error conditions
- EOT (End of Travel) status
- Output enable status
- Active system errors

### Monitoring Data

#### Position Stream
Monitor real-time position data at 10kHz:
```bash
mosquitto_sub -h localhost -t "microscope/stage/position"
```

**Data Format:**
```
timestamp_picoseconds/X_position/Y_position/Z_position/R_position
1735689123456789000/999730/-92564/-224330/-600530
1735689123457789000/999730/-92564/-224330/-600530
```

#### Command Results and Errors
Monitor command execution results and system errors:
```bash
mosquitto_sub -h localhost -t "microscope/stage/result"
```

**Command Result Format:**
```
timestamp_ps/COMMAND/command_id/axis/SUCCESS_or_FAILED/optional_error_message
1735689123456789000/COMMAND/1735689123456789000/X/SUCCESS
1735689123457789000/COMMAND/1735689123457789000/Y/FAILED/Axis not connected
```

**Error Format:**
```
timestamp_ps/ERROR/error_type/axis_or_system/severity/description
1735689123456789000/ERROR/EOT_FORWARD/X/CRITICAL/End of travel detected on X
1735689123457789000/ERROR/MQTT_DISCONNECTED/SYSTEM/ERROR/MQTT broker connection lost
```

#### System Status
Monitor system status changes:
```bash
mosquitto_sub -h localhost -t "microscope/stage/status"
```

**Status Messages:**
- `SYSTEM_READY` - System initialized and ready
- `SYSTEM_SHUTDOWN` - System shutting down

### Architecture

### Multi-Threaded Design

The system uses three main threads for optimal performance:

1. **Position Publisher Thread** (10kHz)
   - Continuously samples all axis positions using controller ID-based lookup
   - Publishes position data via MQTT with picosecond timestamps
   - Maintains precise timing intervals with performance monitoring

2. **Command Receiver Thread** (High Responsiveness)
   - Processes incoming MQTT commands
   - Executes movement and stop commands using ID-based controller mapping
   - Reports command results and failures

3. **Error Monitor Thread** (20Hz)
   - Monitors hardware error conditions
   - Checks for end-of-travel (EOT) conditions
   - Performs emergency stops when needed  
   - Monitors controller and MQTT connectivity

### Controller ID-Based Mapping

The system uses controller IDs rather than connection order for reliable operation:
- **find_controller_by_id()** locates controllers by their internal ID
- **get_controller_axis_from_name()** maps logical axis names (X, Y, Z, R) to physical controller/axis
- **Plug-and-play operation** - works regardless of USB connection order
- **Clear error messages** when expected controllers are missing

### Safety Features

#### Automatic Emergency Stops
The system automatically stops movement when:
- **Hardware errors** detected on any axis
- **End of travel (EOT)** conditions reached
- **Controller disconnection** detected
- **Command execution exceptions** occur

#### Pre-flight Safety Checks
Before executing movement commands:
- Verify controller and axis connectivity using ID-based lookup
- Check for existing hardware errors
- Validate EOT status
- Confirm axis is operational

#### Error Reporting
All errors are:
- Logged to console with severity levels
- Published to MQTT result topic with proper axis identification
- Stored in internal error tracking system
- Timestamped with picosecond precision

### Performance Verification

Check actual performance using the STATUS command:
```bash
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STATUS"
```

Look for these metrics in the response:
- **Sample Rate**: Configured rate (e.g., 10000 Hz)
- **Actual Frequency**: Measured rate (should be within ±0.1% of sample rate)
- **Total Samples**: Number of position samples taken
- **Missed Deadlines**: Count of timing violations (should be < 0.1%)

### Troubleshooting

### Common Issues

#### 1. Wrong Controller Detected
```bash
# Check controller IDs with ecc_tool
./ecc_tool list

# Look for:
# Controller 0 (ID=4, Handle=1)  <- Should be XYZ
# Controller 1 (ID=2222, Handle=2) <- Should be R
```

If IDs don't match, update the constants in the code:
```cpp
const int XYZ_CONTROLLER_ID = 4;     // Update if different
const int R_CONTROLLER_ID = 2222;    // Update if different
```

#### 2. Axis Mapping Issues
The system expects:
- **Controller ID=4**: Y=axis0, X=axis1, Z=axis2
- **Controller ID=2222**: R=axis0

If your hardware has different mapping, update the `get_axis_name()` and related functions.

#### 3. Performance Issues
```bash
# Check actual frequency with STATUS command
mosquitto_pub -h localhost -t "microscope/stage/command" -m "STATUS"

# Monitor system load
htop -p $(pgrep ecc_mqtt_streaming)

# Check for USB issues
dmesg | grep -i usb
```

#### 4. Compilation Errors
```bash
# Missing MQTT library
sudo apt-get install libmosquitto-dev

# Missing ECC100 library
export LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH
sudo ldconfig
```

#### 5. MQTT Connection Issues
```bash
# Check if broker is running
sudo systemctl status mosquitto

# Start broker if stopped
sudo systemctl start mosquitto

# Test broker connectivity
mosquitto_pub -h localhost -t "test" -m "hello"
mosquitto_sub -h localhost -t "test"
```

#### 6. Controller Connection Issues
```bash
# Check if controllers are detected
./ecc_tool list

# Verify USB/Ethernet connections
lsusb  # For USB controllers
ip addr  # For Ethernet controllers

# Check permissions
sudo usermod -a -G dialout $USER
# Log out and back in for changes to take effect
```

### Debug Mode

Enable verbose logging by adding debug output to the source code:

```cpp
// Add this near the top of functions for debugging
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    std::cout << "DEBUG: Function_name - status info\n";
}
```

### Monitoring Tools

#### Real-time Position Monitoring
```bash
# Monitor with timestamps
mosquitto_sub -h localhost -t "microscope/stage/position" -F "%I [%t] %p"

# Count messages to verify frequency
timeout 10s mosquitto_sub -h localhost -t "microscope/stage/position" | wc -l
# Should show ~100,000 for 10kHz over 10 seconds
```

#### System Health Monitoring
```bash
# Monitor all system topics
mosquitto_sub -h localhost -t "microscope/stage/+" -v

# Monitor only errors
mosquitto_sub -h localhost -t "microscope/stage/result" | grep ERROR
```
## Safety Guidelines

### Operational Safety
1. **Always verify controller IDs** before operation using STATUS command
2. **Monitor EOT status** - system will auto-stop but verify manually
3. **Use reasonable movement speeds** - system doesn't control velocity directly
4. **Test with small movements** before large positioning operations
5. **Keep emergency stop accessible** - Ctrl+C stops the entire system

### System Safety
1. **Monitor MQTT connectivity** - loss of connection may isolate system
2. **Check error logs regularly** - watch the result topic for issues
3. **Verify controller connections** before critical operations using STATUS command
4. **Backup position data** if needed for critical applications
5. **Test recovery procedures** - practice restarting after errors

### Data Safety
1. **Position timestamps are in picoseconds** - handle large numbers appropriately
2. **NaN values indicate invalid readings** - check before using position data
3. **Commands are executed immediately** - no undo functionality
4. **System state is not persistent** - positions reset on restart

## Support and Maintenance

### Log Files
The system outputs all information to stdout/stderr. To log to files:

```bash
# Run with logging
./ecc_mqtt_streaming 2>&1 | tee ecc_mqtt_$(date +%Y%m%d_%H%M%S).log

# Run in background with logging
nohup ./ecc_mqtt_streaming > ecc_mqtt.log 2>&1 &
```

### System Monitoring
Create a systemd service for automatic startup:

```ini
# /etc/systemd/system/ecc-mqtt.service
[Unit]
Description=ECC100 MQTT Streaming Control
After=network.target mosquitto.service

[Service]
Type=simple
User=your_username
WorkingDirectory=/path/to/ecc/directory
ExecStart=/path/to/ecc/directory/ecc_mqtt_streaming
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
# Enable and start service
sudo systemctl daemon-reload
sudo systemctl enable ecc-mqtt.service
sudo systemctl start ecc-mqtt.service

# Monitor service
sudo systemctl status ecc-mqtt.service
sudo journalctl -u ecc-mqtt.service -f
```

### Performance Monitoring

```bash
# Monitor system resources
top -p $(pgrep ecc_mqtt_streaming)

# Monitor network usage
sudo netstat -i  # Interface statistics
sudo ss -tuln    # Network connections

# Monitor MQTT traffic
mosquitto_sub -h localhost -t '$SYS/broker/messages/received/1min'
mosquitto_sub -h localhost -t '$SYS/broker/messages/sent/1min'
```
