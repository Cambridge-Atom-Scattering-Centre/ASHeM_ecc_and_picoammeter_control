#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <iomanip>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <string>
#include <queue>
#include <cstring>
#include <array>

// Network includes
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// MQTT includes
#include <mosquitto.h>

// Real-time scheduling
#include <sched.h>
#include <pthread.h>

#ifdef unix
#define __declspec(x)
#define _stdcall
#endif

#include "ecc.h"

// Global configuration (made non-const for runtime changes)
int g_sample_rate_hz = 80;  // Changeable sampling rate
int g_sample_interval_ns = 1000000000 / g_sample_rate_hz;  // Updated dynamically
const int BUFFER_SIZE = 1000;     // Batch size for MQTT publishing
const int TCP_PORT = 8080;
const std::string MQTT_BROKER = "localhost";
const int MQTT_PORT = 1883;
const std::string MQTT_TOPIC_POSITION = "microscope/stage/position";
const std::string MQTT_TOPIC_COMMAND = "microscope/stage/command";
const std::string MQTT_TOPIC_RESULT = "microscope/stage/result";
const std::string MQTT_TOPIC_STATUS = "microscope/stage/status";

// Optimized position data structure (POD for cache efficiency)
struct __attribute__((packed)) PositionSample {
    uint64_t timestamp_ns;     // Nanoseconds since epoch
    int32_t x_position;
    int32_t y_position; 
    int32_t z_position;
    int32_t r_position;
    uint8_t valid_mask;        // Bit flags for valid positions (X=1, Y=2, Z=4, R=8)
    
    PositionSample() : timestamp_ns(0), x_position(0), y_position(0), 
                      z_position(0), r_position(0), valid_mask(0) {}
};

// Lock-free circular buffer for high-speed producer-consumer
class LockFreeBuffer {
private:
    alignas(64) std::array<PositionSample, BUFFER_SIZE * 4> buffer;  // 4x buffer for safety
    alignas(64) std::atomic<size_t> write_pos{0};
    alignas(64) std::atomic<size_t> read_pos{0};
    
public:
    bool try_write(const PositionSample& sample) {
        size_t current_write = write_pos.load(std::memory_order_relaxed);
        size_t next_write = (current_write + 1) % buffer.size();
        
        if (next_write == read_pos.load(std::memory_order_acquire)) {
            return false;  // Buffer full
        }
        
        buffer[current_write] = sample;
        write_pos.store(next_write, std::memory_order_release);
        return true;
    }
    
    bool try_read(PositionSample& sample) {
        size_t current_read = read_pos.load(std::memory_order_relaxed);
        if (current_read == write_pos.load(std::memory_order_acquire)) {
            return false;  // Buffer empty
        }
        
        sample = buffer[current_read];
        read_pos.store((current_read + 1) % buffer.size(), std::memory_order_release);
        return true;
    }
    
    size_t available() const {
        size_t w = write_pos.load(std::memory_order_relaxed);
        size_t r = read_pos.load(std::memory_order_relaxed);
        return (w >= r) ? (w - r) : (buffer.size() - r + w);
    }
};

// Pre-allocated string buffer to avoid malloc in hot path
class FastStringBuffer {
private:
    std::array<char, 256> buffer;
    
public:
    const char* format_position(const PositionSample& sample) {
        // Fast integer-to-string conversion without ostringstream
        int pos = 0;
        
        // Timestamp
        pos += uint64_to_string(sample.timestamp_ns, buffer.data() + pos);
        buffer[pos++] = '/';
        
        // X position
        if (sample.valid_mask & 1) {
            pos += int32_to_string(sample.x_position, buffer.data() + pos);
        } else {
            buffer[pos++] = 'N'; buffer[pos++] = 'a'; buffer[pos++] = 'N';
        }
        buffer[pos++] = '/';
        
        // Y position
        if (sample.valid_mask & 2) {
            pos += int32_to_string(sample.y_position, buffer.data() + pos);
        } else {
            buffer[pos++] = 'N'; buffer[pos++] = 'a'; buffer[pos++] = 'N';
        }
        buffer[pos++] = '/';
        
        // Z position
        if (sample.valid_mask & 4) {
            pos += int32_to_string(sample.z_position, buffer.data() + pos);
        } else {
            buffer[pos++] = 'N'; buffer[pos++] = 'a'; buffer[pos++] = 'N';
        }
        buffer[pos++] = '/';
        
        // R position
        if (sample.valid_mask & 8) {
            pos += int32_to_string(sample.r_position, buffer.data() + pos);
        } else {
            buffer[pos++] = 'N'; buffer[pos++] = 'a'; buffer[pos++] = 'N';
        }
        
        buffer[pos] = '\0';
        return buffer.data();
    }
    
private:
    int uint64_to_string(uint64_t value, char* buf) {
        if (value == 0) {
            buf[0] = '0';
            return 1;
        }
        
        char temp[32];
        int pos = 0;
        while (value > 0) {
            temp[pos++] = '0' + (value % 10);
            value /= 10;
        }
        
        for (int i = 0; i < pos; ++i) {
            buf[i] = temp[pos - 1 - i];
        }
        return pos;
    }
    
    int int32_to_string(int32_t value, char* buf) {
        int pos = 0;
        if (value < 0) {
            buf[pos++] = '-';
            value = -value;
        }
        
        if (value == 0) {
            buf[pos++] = '0';
            return pos;
        }
        
        char temp[16];
        int temp_pos = 0;
        while (value > 0) {
            temp[temp_pos++] = '0' + (value % 10);
            value /= 10;
        }
        
        for (int i = 0; i < temp_pos; ++i) {
            buf[pos++] = temp[temp_pos - 1 - i];
        }
        return pos;
    }
};

// Global variables
std::atomic<bool> g_running(true);
std::atomic<bool> g_controllers_connected(false);
std::atomic<bool> g_mqtt_connected(false);
std::mutex g_command_mutex;
std::queue<std::string> g_command_queue;  // Simplified command structure
std::mutex g_error_mutex;

// High-performance buffers
LockFreeBuffer g_position_buffer;
thread_local FastStringBuffer g_string_buffer;

// MQTT client
struct mosquitto *g_mqtt_client = nullptr;

// Controller handles
struct ControllerInfo {
    int handle = -1;
    int id = -1;
    bool connected = false;
    std::array<bool, 3> axes_connected = {false, false, false};
};

std::array<ControllerInfo, 2> g_controllers;  // Fixed-size array for cache efficiency

// Performance statistics
std::atomic<uint64_t> g_samples_captured{0};
std::atomic<uint64_t> g_samples_published{0};
std::atomic<uint64_t> g_samples_dropped{0};
std::atomic<uint64_t> g_total_captured{0};
std::atomic<uint64_t> g_total_published{0};
std::atomic<uint64_t> g_total_dropped{0};

// Function prototypes
bool initialize_controllers();
void cleanup_controllers();
void high_speed_sampler_thread();      // Thread 1: Ultra-fast sampling
void batch_publisher_thread();         // Thread 2: Batched MQTT publishing  
void command_processor_thread();       // Thread 3: Command processing
bool initialize_mqtt();
void cleanup_mqtt();
void mqtt_on_connect(struct mosquitto *mosq, void *userdata, int result);
void mqtt_on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message);
void mqtt_on_disconnect(struct mosquitto *mosq, void *userdata, int rc);
PositionSample read_all_positions_fast();
uint64_t get_nanosecond_timestamp();
std::string get_axis_name(int controller, int axis);

// Optimized utility functions
inline uint64_t get_nanosecond_timestamp() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

std::string get_axis_name(int controller, int axis) {
    if (controller == 0) {
        if (axis == 0) return "X";
        else if (axis == 1) return "Y";
        else if (axis == 2) return "Z";
    } else if (controller == 1 && axis == 0) {
        return "R";
    }
    return "UNKNOWN";
}

// High-speed position reading (optimized for cache efficiency)
PositionSample read_all_positions_fast() {
    PositionSample sample;
    sample.timestamp_ns = get_nanosecond_timestamp();
    
    // Controller 0: X(axis0), Y(axis1), Z(axis2)
    if (g_controllers[0].connected) {
        Int32 pos;
        if (g_controllers[0].axes_connected[0] && ECC_getPosition(g_controllers[0].handle, 0, &pos) == 0) {
            sample.x_position = pos;
            sample.valid_mask |= 1;
        }
        if (g_controllers[0].axes_connected[1] && ECC_getPosition(g_controllers[0].handle, 1, &pos) == 0) {
            sample.y_position = pos;
            sample.valid_mask |= 2;
        }
        if (g_controllers[0].axes_connected[2] && ECC_getPosition(g_controllers[0].handle, 2, &pos) == 0) {
            sample.z_position = pos;
            sample.valid_mask |= 4;
        }
    }
    
    // Controller 1: R(axis0)
    if (g_controllers[1].connected && g_controllers[1].axes_connected[0]) {
        Int32 pos;
        if (ECC_getPosition(g_controllers[1].handle, 0, &pos) == 0) {
            sample.r_position = pos;
            sample.valid_mask |= 8;
        }
    }
    
    return sample;
}

// Ultra-high-speed sampling thread with real-time priority
void high_speed_sampler_thread() {
    std::cout << "High-speed sampler thread started (" << g_sample_rate_hz << " Hz)\n";
    
    // Set real-time priority
    struct sched_param param;
    param.sched_priority = 50;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
        std::cout << "Real-time scheduling enabled for sampler\n";
    } else {
        std::cout << "Warning: Could not enable real-time scheduling\n";
    }
    
    // Pin to specific CPU core (optional)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);  // Use CPU core 1
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    const auto target_interval = std::chrono::nanoseconds(g_sample_interval_ns);
    auto next_sample_time = std::chrono::high_resolution_clock::now();
    
    uint64_t sample_count = 0;
    uint64_t dropped_count = 0;
    uint64_t debug_counter = 0;
    
    while (g_running && g_controllers_connected) {
        // Read positions (extremely fast - ~50ns)
        PositionSample sample = read_all_positions_fast();
        
        // Debug output every 10000 samples (减少频率)
        if (++debug_counter % 10000 == 0) {
            std::cout << "Sampler: " << debug_counter << " samples processed\n";
        }
        
        // Try to write to lock-free buffer
        if (g_position_buffer.try_write(sample)) {
            sample_count++;
            g_total_captured.fetch_add(1, std::memory_order_relaxed);
        } else {
            dropped_count++;
            g_total_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Precise timing control
        next_sample_time += target_interval;
        
        // Busy wait for precision (last few microseconds)
        auto now = std::chrono::high_resolution_clock::now();
        if (now < next_sample_time) {
            // Hybrid sleep: coarse sleep + fine busy-wait
            auto sleep_time = next_sample_time - now;
            if (sleep_time > std::chrono::microseconds(100)) {
                std::this_thread::sleep_for(sleep_time - std::chrono::microseconds(50));
            }
            
            // Fine busy-wait for precision
            while (std::chrono::high_resolution_clock::now() < next_sample_time) {
                std::this_thread::yield();
            }
        }
    }
    
    g_samples_captured = sample_count;
    g_samples_dropped = dropped_count;
    std::cout << "Sampler thread stopped. Captured: " << sample_count 
              << ", Dropped: " << dropped_count << "\n";
}

// Batched MQTT publishing thread
void batch_publisher_thread() {
    std::cout << "Batch publisher thread started\n";
    
    std::vector<PositionSample> batch;
    batch.reserve(BUFFER_SIZE);
    
    uint64_t published_count = 0;
    uint64_t batch_count = 0;
    const auto batch_interval = std::chrono::milliseconds(100);  // 10Hz batch rate for easier debugging
    auto next_batch_time = std::chrono::steady_clock::now() + batch_interval;
    
    while (g_running) {
        // Collect batch of samples
        PositionSample sample;
        while (batch.size() < BUFFER_SIZE && g_position_buffer.try_read(sample)) {
            batch.push_back(sample);
        }
        
        // Publish batch if not empty
        if (!batch.empty()) {
            batch_count++;
            // 减少发布成功的输出频率
            if (batch_count % 50 == 0) {
                std::cout << "Published batch " << batch_count << " (total: " << published_count << " samples)\n";
            }
            
            if (g_mqtt_connected) {
                // Create batched message (more efficient than individual messages)
                std::ostringstream batch_msg;
                
                for (size_t i = 0; i < batch.size(); ++i) {
                    const char* formatted = g_string_buffer.format_position(batch[i]);
                    batch_msg << formatted;
                    if (i < batch.size() - 1) {
                        batch_msg << "\n";  // Separate samples with newlines
                    }
                }
                
                std::string msg = batch_msg.str();
                int rc = mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_POSITION.c_str(), 
                                         msg.length(), msg.c_str(), 0, false);
                
                if (rc == MOSQ_ERR_SUCCESS) {
                    published_count += batch.size();
                    g_total_published.fetch_add(batch.size(), std::memory_order_relaxed);
                } else {
                    std::cout << "Failed to publish batch: " << mosquitto_strerror(rc) << "\n";
                }
            } else {
                std::cout << "MQTT not connected, skipping batch\n";
            }
            
            batch.clear();
        }
        
        // Wait for next batch interval
        std::this_thread::sleep_until(next_batch_time);
        next_batch_time += batch_interval;
    }
    
    g_samples_published = published_count;
    std::cout << "Publisher thread stopped. Published: " << published_count << "\n";
}

// Simplified command processing thread
void command_processor_thread() {
    std::cout << "Command processor thread started\n";
    
    while (g_running) {
        std::string cmd;
        bool has_command = false;
        
        {
            std::lock_guard<std::mutex> lock(g_command_mutex);
            if (!g_command_queue.empty()) {
                cmd = g_command_queue.front();
                g_command_queue.pop();
                has_command = true;
            }
        }
        
        if (has_command) {
            std::cout << "Processing command: " << cmd << "\n";
            
            // Parse and execute commands
            if (cmd == "STATUS") {
                // Generate status report with proper formatting
                std::ostringstream status;
                status << "=== ECC100 MQTT System Status ===\n";
                status << "MQTT Connected: " << (g_mqtt_connected ? "YES" : "NO") << "\n";
                status << "Controllers Connected: " << (g_controllers_connected ? "YES" : "NO") << "\n";
                status << "Sample Rate: " << g_sample_rate_hz << " Hz\n";
                status << "Total Captured: " << g_total_captured.load() << "\n";
                status << "Total Published: " << g_total_published.load() << "\n";
                status << "Total Dropped: " << g_total_dropped.load() << "\n";
                status << "Buffer Usage: " << g_position_buffer.available() << "/" << (BUFFER_SIZE * 4) << "\n\n";
                
                // Controller details with amplitude and frequency
                for (int i = 0; i < 2; ++i) {
                    if (g_controllers[i].connected) {
                        status << "Controller " << i << " (ID=" << g_controllers[i].id << ")\n";
                        
                        // Get firmware version
                        Int32 firmware_version = 0;
                        if (ECC_getFirmwareVersion(g_controllers[i].handle, &firmware_version) == 0) {
                            status << "  Firmware Version: " << firmware_version << "\n";
                        }
                        
                        for (int axis = 0; axis < 3; ++axis) {
                            if (g_controllers[i].axes_connected[axis]) {
                                std::string axis_name = get_axis_name(i, axis);
                                status << "  Axis " << axis << " (" << axis_name << "):";
                                
                                // Current position
                                Int32 position = 0;
                                if (ECC_getPosition(g_controllers[i].handle, axis, &position) == 0) {
                                    status << " " << position;
                                    
                                    // Get actor type for units
                                    ECC_actorType actor_type;
                                    if (ECC_getActorType(g_controllers[i].handle, axis, &actor_type) == 0) {
                                        switch (actor_type) {
                                            case ECC_actorLinear:
                                                status << " nm [Linear]";
                                                break;
                                            case ECC_actorGonio:
                                                status << " µ° [Goniometer]";
                                                break;
                                            case ECC_actorRot:
                                                status << " µ° [Rotator]";
                                                break;
                                        }
                                    }
                                    
                                    // Get actor name
                                    char actor_name[20] = {0};
                                    if (ECC_getActorName(g_controllers[i].handle, axis, actor_name) == 0) {
                                        status << " (" << actor_name << ")";
                                    }
                                }
                                status << "\n";
                                
                                // Amplitude and frequency
                                Int32 amplitude = 0, frequency = 0;
                                if (ECC_controlAmplitude(g_controllers[i].handle, axis, &amplitude, 0) == 0) {
                                    status << "    Amplitude: " << amplitude << " mV\n";
                                }
                                if (ECC_controlFrequency(g_controllers[i].handle, axis, &frequency, 0) == 0) {
                                    status << "    Frequency: " << frequency << " mHz\n";
                                }
                                
                                // Target range
                                Int32 target_range = 0;
                                if (ECC_controlTargetRange(g_controllers[i].handle, axis, &target_range, 0) == 0) {
                                    status << "    Target Range: " << target_range << " nm/µ°\n";
                                }
                                
                                // Status flags
                                Bln32 ref_valid = 0, moving = 0, in_target = 0;
                                if (ECC_getStatusReference(g_controllers[i].handle, axis, &ref_valid) == 0) {
                                    status << "    Reference Valid: " << (ref_valid ? "YES" : "NO");
                                    if (ref_valid) {
                                        Int32 ref_pos = 0;
                                        if (ECC_getReferencePosition(g_controllers[i].handle, axis, &ref_pos) == 0) {
                                            status << " (Position: " << ref_pos << ")";
                                        }
                                    }
                                    status << "\n";
                                }
                                
                                if (ECC_getStatusMoving(g_controllers[i].handle, axis, &moving) == 0) {
                                    status << "    Moving Status: ";
                                    switch (moving) {
                                        case 0: status << "IDLE"; break;
                                        case 1: status << "MOVING"; break;
                                        case 2: status << "PENDING"; break;
                                        default: status << "UNKNOWN(" << moving << ")"; break;
                                    }
                                    status << "\n";
                                }
                                
                                if (ECC_getStatusTargetRange(g_controllers[i].handle, axis, &in_target) == 0) {
                                    status << "    In Target Range: " << (in_target ? "YES" : "NO") << "\n";
                                }
                                
                                // EOT status
                                Bln32 eot_fwd = 0, eot_bkwd = 0;
                                if (ECC_getStatusEotFwd(g_controllers[i].handle, axis, &eot_fwd) == 0) {
                                    status << "    EOT Forward: " << (eot_fwd ? "DETECTED" : "Clear") << "\n";
                                }
                                if (ECC_getStatusEotBkwd(g_controllers[i].handle, axis, &eot_bkwd) == 0) {
                                    status << "    EOT Backward: " << (eot_bkwd ? "DETECTED" : "Clear") << "\n";
                                }
                                
                                status << "\n";  // Extra line between axes
                            }
                        }
                        status << "\n";  // Extra line between controllers
                    }
                }
                
                // Publish status to MQTT result topic
                if (g_mqtt_connected) {
                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                    std::string result_msg = timestamp + "/STATUS/SYSTEM_INFO/ALL/SUCCESS/" + status.str();
                    
                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                    result_msg.length(), result_msg.c_str(), 1, false);
                    
                    std::cout << "Status report published to MQTT result topic\n";
                }
                
            } else if (cmd.find("SET_RATE/") == 0) {
                // Handle SET_RATE command: "SET_RATE/8000"
                std::istringstream iss(cmd);
                std::string rate_cmd, rate_str;
                
                if (std::getline(iss, rate_cmd, '/') && std::getline(iss, rate_str)) {
                    int new_rate = std::atoi(rate_str.c_str());
                    
                    if (new_rate >= 100 && new_rate <= 15000) {  // Reasonable limits
                        g_sample_rate_hz = new_rate;
                        g_sample_interval_ns = 1000000000 / g_sample_rate_hz;
                        
                        std::cout << "Sampling rate changed to " << g_sample_rate_hz << " Hz\n";
                        
                        // Publish success result
                        if (g_mqtt_connected) {
                            std::string timestamp = std::to_string(get_nanosecond_timestamp());
                            std::string result_msg = timestamp + "/COMMAND/SET_RATE/ALL/SUCCESS/Sampling rate set to " + rate_str + " Hz";
                            
                            mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                            result_msg.length(), result_msg.c_str(), 1, false);
                        }
                    } else {
                        std::cout << "Invalid sampling rate: " << new_rate << " (must be 100-15000 Hz)\n";
                        
                        // Publish error result
                        if (g_mqtt_connected) {
                            std::string timestamp = std::to_string(get_nanosecond_timestamp());
                            std::string result_msg = timestamp + "/COMMAND/SET_RATE/ALL/FAILED/Invalid rate (must be 100-15000 Hz)";
                            
                            mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                            result_msg.length(), result_msg.c_str(), 1, false);
                        }
                    }
                } else {
                    std::cout << "Invalid SET_RATE command format: " << cmd << "\n";
                }
                
            } else if (cmd.find("SET_AMP/") == 0) {
                // Handle SET_AMP command: "SET_AMP/X/45000"
                std::istringstream iss(cmd);
                std::string amp_cmd, axis_str, amp_str;
                
                if (std::getline(iss, amp_cmd, '/') && 
                    std::getline(iss, axis_str, '/') && 
                    std::getline(iss, amp_str)) {
                    
                    int32_t amplitude = std::atoi(amp_str.c_str());
                    std::cout << "Set amplitude command: " << axis_str << " to " << amplitude << " mV\n";
                    
                    // Map axis names to controller/axis numbers
                    int controller = -1, axis = -1;
                    bool valid_axis = false;
                    
                    if (axis_str == "X") { controller = 0; axis = 0; valid_axis = true; }
                    else if (axis_str == "Y") { controller = 0; axis = 1; valid_axis = true; }
                    else if (axis_str == "Z") { controller = 0; axis = 2; valid_axis = true; }
                    else if (axis_str == "R") { controller = 1; axis = 0; valid_axis = true; }
                    
                    if (valid_axis && controller >= 0 && axis >= 0) {
                        if (g_controllers[controller].connected && 
                            g_controllers[controller].axes_connected[axis]) {
                            
                            // Set amplitude
                            Int32 amp = amplitude;
                            int result = ECC_controlAmplitude(g_controllers[controller].handle, axis, &amp, 1);
                            
                            if (result == 0) {
                                std::cout << "Successfully set amplitude: " << axis_str << " = " << amplitude << " mV\n";
                                
                                // Publish success result
                                if (g_mqtt_connected) {
                                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                    std::string result_msg = timestamp + "/COMMAND/SET_AMP/" + axis_str + "/SUCCESS/Amplitude set to " + amp_str + " mV";
                                    
                                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                    result_msg.length(), result_msg.c_str(), 1, false);
                                }
                            } else {
                                std::cout << "Failed to set amplitude for " << axis_str << "\n";
                                
                                // Publish failure result
                                if (g_mqtt_connected) {
                                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                    std::string result_msg = timestamp + "/COMMAND/SET_AMP/" + axis_str + "/FAILED/Failed to set amplitude";
                                    
                                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                    result_msg.length(), result_msg.c_str(), 1, false);
                                }
                            }
                        } else {
                            std::cout << "Axis " << axis_str << " not connected\n";
                        }
                    } else {
                        std::cout << "Invalid axis for SET_AMP: " << axis_str << "\n";
                    }
                } else {
                    std::cout << "Invalid SET_AMP command format: " << cmd << "\n";
                }
                
            } else if (cmd.find("SET_FREQ/") == 0) {
                // Handle SET_FREQ command: "SET_FREQ/X/1000000"
                std::istringstream iss(cmd);
                std::string freq_cmd, axis_str, freq_str;
                
                if (std::getline(iss, freq_cmd, '/') && 
                    std::getline(iss, axis_str, '/') && 
                    std::getline(iss, freq_str)) {
                    
                    int32_t frequency = std::atoi(freq_str.c_str());
                    std::cout << "Set frequency command: " << axis_str << " to " << frequency << " mHz\n";
                    
                    // Map axis names to controller/axis numbers
                    int controller = -1, axis = -1;
                    bool valid_axis = false;
                    
                    if (axis_str == "X") { controller = 0; axis = 0; valid_axis = true; }
                    else if (axis_str == "Y") { controller = 0; axis = 1; valid_axis = true; }
                    else if (axis_str == "Z") { controller = 0; axis = 2; valid_axis = true; }
                    else if (axis_str == "R") { controller = 1; axis = 0; valid_axis = true; }
                    
                    if (valid_axis && controller >= 0 && axis >= 0) {
                        if (g_controllers[controller].connected && 
                            g_controllers[controller].axes_connected[axis]) {
                            
                            // Set frequency
                            Int32 freq = frequency;
                            int result = ECC_controlFrequency(g_controllers[controller].handle, axis, &freq, 1);
                            
                            if (result == 0) {
                                std::cout << "Successfully set frequency: " << axis_str << " = " << frequency << " mHz\n";
                                
                                // Publish success result
                                if (g_mqtt_connected) {
                                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                    std::string result_msg = timestamp + "/COMMAND/SET_FREQ/" + axis_str + "/SUCCESS/Frequency set to " + freq_str + " mHz";
                                    
                                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                    result_msg.length(), result_msg.c_str(), 1, false);
                                }
                            } else {
                                std::cout << "Failed to set frequency for " << axis_str << "\n";
                                
                                // Publish failure result
                                if (g_mqtt_connected) {
                                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                    std::string result_msg = timestamp + "/COMMAND/SET_FREQ/" + axis_str + "/FAILED/Failed to set frequency";
                                    
                                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                    result_msg.length(), result_msg.c_str(), 1, false);
                                }
                            }
                        } else {
                            std::cout << "Axis " << axis_str << " not connected\n";
                        }
                    } else {
                        std::cout << "Invalid axis for SET_FREQ: " << axis_str << "\n";
                    }
                } else {
                    std::cout << "Invalid SET_FREQ command format: " << cmd << "\n";
                }
                
            } else if (cmd.find("MOVE/") == 0) {
                // Handle MOVE commands: "MOVE/X/1000" or "MOVE/Y/-500"
                std::istringstream iss(cmd);
                std::string move_cmd, axis_str, pos_str;
                
                if (std::getline(iss, move_cmd, '/') && 
                    std::getline(iss, axis_str, '/') && 
                    std::getline(iss, pos_str)) {
                    
                    int32_t target_position = std::atoi(pos_str.c_str());
                    std::cout << "Move command: " << axis_str << " to " << target_position << "\n";
                    
                    // Map axis names to controller/axis numbers
                    int controller = -1, axis = -1;
                    bool valid_axis = false;
                    
                    if (axis_str == "X") { controller = 0; axis = 0; valid_axis = true; }
                    else if (axis_str == "Y") { controller = 0; axis = 1; valid_axis = true; }
                    else if (axis_str == "Z") { controller = 0; axis = 2; valid_axis = true; }
                    else if (axis_str == "R") { controller = 1; axis = 0; valid_axis = true; }
                    
                    if (valid_axis && controller >= 0 && axis >= 0) {
                        // Check if controller and axis are available
                        if (g_controllers[controller].connected && 
                            g_controllers[controller].axes_connected[axis]) {
                            
                            // Execute movement
                            std::cout << "Executing move: Controller " << controller 
                                      << " Axis " << axis << " -> " << target_position << "\n";
                            
                            // Set target position
                            Int32 target = target_position;
                            int result1 = ECC_controlTargetPosition(g_controllers[controller].handle, axis, &target, 1);
                            
                            if (result1 == 0) {
                                // Enable movement
                                Bln32 enable = 1;
                                int result2 = ECC_controlMove(g_controllers[controller].handle, axis, &enable, 1);
                                
                                if (result2 == 0) {
                                    std::cout << "Successfully started movement: " << axis_str << " -> " << target_position << "\n";
                                    
                                    // Publish success result
                                    if (g_mqtt_connected) {
                                        std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                        std::string result_msg = timestamp + "/COMMAND/MOVE/" + axis_str + "/SUCCESS/Movement started to " + pos_str;
                                        
                                        mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                        result_msg.length(), result_msg.c_str(), 1, false);
                                    }
                                } else {
                                    std::cout << "Failed to enable movement for " << axis_str << "\n";
                                    
                                    // Publish failure result
                                    if (g_mqtt_connected) {
                                        std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                        std::string result_msg = timestamp + "/COMMAND/MOVE/" + axis_str + "/FAILED/Failed to enable movement";
                                        
                                        mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                        result_msg.length(), result_msg.c_str(), 1, false);
                                    }
                                }
                            } else {
                                std::cout << "Failed to set target position for " << axis_str << "\n";
                                
                                // Publish failure result
                                if (g_mqtt_connected) {
                                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                    std::string result_msg = timestamp + "/COMMAND/MOVE/" + axis_str + "/FAILED/Failed to set target position";
                                    
                                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                    result_msg.length(), result_msg.c_str(), 1, false);
                                }
                            }
                        } else {
                            std::cout << "Axis " << axis_str << " not connected or controller not available\n";
                            
                            // Publish error result
                            if (g_mqtt_connected) {
                                std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                std::string result_msg = timestamp + "/COMMAND/MOVE/" + axis_str + "/FAILED/Axis not connected";
                                
                                mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                result_msg.length(), result_msg.c_str(), 1, false);
                            }
                        }
                    } else {
                        std::cout << "Invalid axis: " << axis_str << "\n";
                        
                        // Publish error result
                        if (g_mqtt_connected) {
                            std::string timestamp = std::to_string(get_nanosecond_timestamp());
                            std::string result_msg = timestamp + "/COMMAND/MOVE/" + axis_str + "/FAILED/Invalid axis name";
                            
                            mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                            result_msg.length(), result_msg.c_str(), 1, false);
                        }
                    }
                } else {
                    std::cout << "Invalid MOVE command format: " << cmd << "\n";
                }
                
            } else if (cmd.find("STOP/") == 0) {
                // Handle STOP commands: "STOP/X" or "STOP/Y"
                std::istringstream iss(cmd);
                std::string stop_cmd, axis_str;
                
                if (std::getline(iss, stop_cmd, '/') && std::getline(iss, axis_str)) {
                    std::cout << "Stop command: " << axis_str << "\n";
                    
                    // Map axis names to controller/axis numbers
                    int controller = -1, axis = -1;
                    bool valid_axis = false;
                    
                    if (axis_str == "X") { controller = 0; axis = 0; valid_axis = true; }
                    else if (axis_str == "Y") { controller = 0; axis = 1; valid_axis = true; }
                    else if (axis_str == "Z") { controller = 0; axis = 2; valid_axis = true; }
                    else if (axis_str == "R") { controller = 1; axis = 0; valid_axis = true; }
                    
                    if (valid_axis && controller >= 0 && axis >= 0) {
                        if (g_controllers[controller].connected && 
                            g_controllers[controller].axes_connected[axis]) {
                            
                            // Stop movement
                            Bln32 disable = 0;
                            int result = ECC_controlMove(g_controllers[controller].handle, axis, &disable, 1);
                            
                            if (result == 0) {
                                std::cout << "Successfully stopped axis " << axis_str << "\n";
                                
                                // Publish success result
                                if (g_mqtt_connected) {
                                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                    std::string result_msg = timestamp + "/COMMAND/STOP/" + axis_str + "/SUCCESS/Movement stopped";
                                    
                                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                    result_msg.length(), result_msg.c_str(), 1, false);
                                }
                            } else {
                                std::cout << "Failed to stop axis " << axis_str << "\n";
                                
                                // Publish failure result
                                if (g_mqtt_connected) {
                                    std::string timestamp = std::to_string(get_nanosecond_timestamp());
                                    std::string result_msg = timestamp + "/COMMAND/STOP/" + axis_str + "/FAILED/Failed to stop movement";
                                    
                                    mosquitto_publish(g_mqtt_client, nullptr, MQTT_TOPIC_RESULT.c_str(), 
                                                    result_msg.length(), result_msg.c_str(), 1, false);
                                }
                            }
                        } else {
                            std::cout << "Axis " << axis_str << " not connected\n";
                        }
                    } else {
                        std::cout << "Invalid axis for STOP: " << axis_str << "\n";
                    }
                } else {
                    std::cout << "Invalid STOP command format: " << cmd << "\n";
                }
                
            } else {
                std::cout << "Unknown command: " << cmd << "\n";
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "Command processor thread stopped\n";
}

bool initialize_mqtt() {
    mosquitto_lib_init();
    
    g_mqtt_client = mosquitto_new(nullptr, true, nullptr);
    if (!g_mqtt_client) {
        std::cerr << "Failed to create MQTT client\n";
        return false;
    }

    mosquitto_connect_callback_set(g_mqtt_client, mqtt_on_connect);
    mosquitto_message_callback_set(g_mqtt_client, mqtt_on_message);
    mosquitto_disconnect_callback_set(g_mqtt_client, mqtt_on_disconnect);

    int rc = mosquitto_connect(g_mqtt_client, MQTT_BROKER.c_str(), MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to connect to MQTT broker: " << mosquitto_strerror(rc) << "\n";
        mosquitto_destroy(g_mqtt_client);
        return false;
    }

    rc = mosquitto_loop_start(g_mqtt_client);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to start MQTT loop: " << mosquitto_strerror(rc) << "\n";
        mosquitto_destroy(g_mqtt_client);
        return false;
    }

    // Wait for connection
    int wait_count = 0;
    while (!g_mqtt_connected && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    if (!g_mqtt_connected) {
        std::cerr << "MQTT connection timeout\n";
        return false;
    }

    std::cout << "MQTT connected successfully\n";
    return true;
}

void cleanup_mqtt() {
    if (g_mqtt_client) {
        mosquitto_loop_stop(g_mqtt_client, true);
        mosquitto_disconnect(g_mqtt_client);
        mosquitto_destroy(g_mqtt_client);
    }
    mosquitto_lib_cleanup();
}

void mqtt_on_connect(struct mosquitto *mosq, void * /* userdata */, int result) {
    if (result == 0) {
        g_mqtt_connected = true;
        std::cout << "MQTT connected to broker\n";
        mosquitto_subscribe(mosq, nullptr, MQTT_TOPIC_COMMAND.c_str(), 0);
        std::cout << "Subscribed to: " << MQTT_TOPIC_COMMAND << "\n";
    } else {
        std::cerr << "MQTT connection failed: " << result << "\n";
    }
}

void mqtt_on_message(struct mosquitto * /* mosq */, void * /* userdata */, const struct mosquitto_message *message) {
    if (!message->payload) return;
    
    std::string payload((char*)message->payload, message->payloadlen);
    
    {
        std::lock_guard<std::mutex> lock(g_command_mutex);
        g_command_queue.push(payload);
    }
}

void mqtt_on_disconnect(struct mosquitto * /* mosq */, void * /* userdata */, int rc) {
    g_mqtt_connected = false;
    if (rc != 0) {
        std::cerr << "MQTT unexpected disconnection\n";
    }
}

bool initialize_controllers() {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    
    if (num_controllers <= 0) {
        std::cerr << "No controllers found\n";
        ECC_ReleaseInfo();
        return false;
    }

    std::cout << "Found " << num_controllers << " controller(s):\n";

    for (int i = 0; i < std::min(num_controllers, 2); ++i) {
        Int32 id, handle;
        Bln32 locked;
        
        if (ECC_getDeviceInfo(i, &id, &locked) != 0) continue;
        if (locked) continue;
        if (ECC_Connect(i, &handle) != 0) continue;

        g_controllers[i].handle = handle;
        g_controllers[i].id = id;
        g_controllers[i].connected = true;

        for (int axis = 0; axis < 3; ++axis) {
            Bln32 connected = 0;
            if (ECC_getStatusConnected(handle, axis, &connected) == 0 && connected) {
                g_controllers[i].axes_connected[axis] = true;
                
                Bln32 enable = 1;
                ECC_controlOutput(handle, axis, &enable, 1);
                
                std::cout << "  Controller " << i << " Axis " << axis << " connected\n";
            }
        }
    }

    ECC_ReleaseInfo();
    g_controllers_connected = true;
    return true;
}

void cleanup_controllers() {
    for (auto& controller : g_controllers) {
        if (controller.connected && controller.handle != -1) {
            for (int axis = 0; axis < 3; ++axis) {
                if (controller.axes_connected[axis]) {
                    Bln32 disable = 0;
                    ECC_controlMove(controller.handle, axis, &disable, 1);
                    ECC_controlOutput(controller.handle, axis, &disable, 1);
                }
            }
            ECC_Close(controller.handle);
        }
    }
}

int main(int /* argc */, char* /* argv */[]) {
    std::cout << "Optimized ECC100 High-Frequency MQTT System\n";
    std::cout << "==========================================\n";
    std::cout << "Target Rate: " << g_sample_rate_hz << " Hz\n";
    std::cout << "Buffer Size: " << BUFFER_SIZE << " samples\n";
    std::cout << "MQTT Broker: " << MQTT_BROKER << ":" << MQTT_PORT << "\n\n";

    if (!initialize_mqtt()) {
        std::cerr << "Failed to initialize MQTT. Exiting.\n";
        return 1;
    }

    if (!initialize_controllers()) {
        std::cerr << "Failed to initialize controllers. Exiting.\n";
        cleanup_mqtt();
        return 1;
    }

    // Start optimized threads
    std::vector<std::thread> threads;
    
    threads.emplace_back(high_speed_sampler_thread);   // Real-time sampling
    threads.emplace_back(batch_publisher_thread);      // Batched publishing
    threads.emplace_back(command_processor_thread);    // Command processing

    std::cout << "All threads started. System ready for high-frequency operation.\n";
    std::cout << "Press Ctrl+C to stop.\n\n";

    // Performance monitoring
    auto last_stats = std::chrono::steady_clock::now();
    uint64_t last_captured = 0;
    uint64_t last_published = 0;
    uint64_t last_dropped = 0;
    
    try {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            // Print performance statistics
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count();
            
            if (elapsed >= 5) {
                uint64_t captured = g_total_captured.load();
                uint64_t published = g_total_published.load();
                uint64_t dropped = g_total_dropped.load();
                size_t buffer_used = g_position_buffer.available();
                
                uint64_t captured_delta = captured - last_captured;
                uint64_t published_delta = published - last_published;
                uint64_t dropped_delta = dropped - last_dropped;
                
                std::cout << "Performance Stats:\n";
                std::cout << "  Captured: " << captured_delta << " samples (" << (captured_delta/elapsed) << " Hz)\n";
                std::cout << "  Published: " << published_delta << " samples (" << (published_delta/elapsed) << " Hz)\n";
                std::cout << "  Dropped: " << dropped_delta << " samples\n";
                std::cout << "  Buffer Usage: " << buffer_used << "/" << (BUFFER_SIZE * 4) << "\n";
                std::cout << "  Total: C=" << captured << ", P=" << published << ", D=" << dropped << "\n\n";
                
                last_stats = now;
                last_captured = captured;
                last_published = published;
                last_dropped = dropped;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "Exception caught: " << e.what() << "\n";
    }

    std::cout << "\nShutting down system...\n";
    g_running = false;

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    cleanup_controllers();
    cleanup_mqtt();
    std::cout << "Shutdown complete.\n";
    return 0;
}
