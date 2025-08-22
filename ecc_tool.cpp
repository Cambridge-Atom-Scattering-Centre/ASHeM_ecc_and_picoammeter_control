#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <iomanip>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include "ecc.h"

void list_controllers();
void move_axis(int stage_index, int axis, int position);
void show_axis_config(Int32 handle, int axis);
void calibrate_axis(int stage_index, int axis);
void continuous_move(int stage_index, int axis, bool forward, int duration_ms = 1000);
void single_step_move(int stage_index, int axis, bool backward, int steps = 1);
void monitor_position(int stage_index, int axis, int duration_seconds = 10);
void set_axis_parameters(int stage_index, int axis, int amplitude = -1, int frequency = -1);
void stop_movement(int stage_index, int axis);
void save_configuration(int stage_index);

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Enhanced ECC100 Control Tool\n"
                  << "Usage:\n"
                  << "  " << argv[0] << " list\n"
                  << "  " << argv[0] << " move <stage_index> <axis> <position>\n"
                  << "  " << argv[0] << " calibrate <stage_index> <axis>\n"
                  << "  " << argv[0] << " continuous <stage_index> <axis> <forward|backward> [duration_ms]\n"
                  << "  " << argv[0] << " step <stage_index> <axis> <forward|backward> [num_steps]\n"
                  << "  " << argv[0] << " monitor <stage_index> <axis> [duration_seconds]\n"
                  << "  " << argv[0] << " config <stage_index> <axis> [amplitude_mV] [frequency_mHz]\n"
                  << "  " << argv[0] << " stop <stage_index> <axis>\n"
                  << "  " << argv[0] << " save <stage_index>\n";
        return 1;
    }

    std::string command = argv[1];

    try {
        if (command == "list") {
            list_controllers();
        } else if (command == "move" && argc >= 5) {
            move_axis(std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]));
        } else if (command == "calibrate" && argc >= 4) {
            calibrate_axis(std::atoi(argv[2]), std::atoi(argv[3]));
        } else if (command == "continuous" && argc >= 5) {
            bool forward = (std::string(argv[4]) == "forward");
            int duration = (argc >= 6) ? std::atoi(argv[5]) : 1000;
            continuous_move(std::atoi(argv[2]), std::atoi(argv[3]), forward, duration);
        } else if (command == "step" && argc >= 5) {
            bool backward = (std::string(argv[4]) == "backward");
            int steps = (argc >= 6) ? std::atoi(argv[5]) : 1;
            single_step_move(std::atoi(argv[2]), std::atoi(argv[3]), backward, steps);
        } else if (command == "monitor" && argc >= 4) {
            int duration = (argc >= 5) ? std::atoi(argv[4]) : 10;
            monitor_position(std::atoi(argv[2]), std::atoi(argv[3]), duration);
        } else if (command == "config" && argc >= 4) {
            int amplitude = (argc >= 5) ? std::atoi(argv[4]) : -1;
            int frequency = (argc >= 6) ? std::atoi(argv[5]) : -1;
            set_axis_parameters(std::atoi(argv[2]), std::atoi(argv[3]), amplitude, frequency);
        } else if (command == "stop" && argc >= 4) {
            stop_movement(std::atoi(argv[2]), std::atoi(argv[3]));
        } else if (command == "save" && argc >= 3) {
            save_configuration(std::atoi(argv[2]));
        } else {
            std::cerr << "Invalid command or insufficient arguments\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

void list_controllers() {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0) {
        std::cerr << "No controllers found.\n";
        return;
    }

    std::cout << "Found " << num_controllers << " controller(s):\n\n";

    for (int i = 0; i < num_controllers; ++i) {
        Int32 id, handle;
        Bln32 locked;
        if (ECC_getDeviceInfo(i, &id, &locked) != 0) {
            std::cerr << "Failed to get device info for controller " << i << "\n";
            continue;
        }
        if (ECC_Connect(i, &handle) != 0) {
            std::cerr << "Failed to connect to controller " << i << "\n";
            continue;
        }

        // Get firmware version
        Int32 firmware_version = 0;
        ECC_getFirmwareVersion(handle, &firmware_version);

        std::cout << "Controller " << i << " (ID=" << id << ", Handle=" << handle;
        if (locked) std::cout << " [LOCKED]";
        std::cout << ")\n";
        std::cout << "Firmware Version: " << firmware_version << "\n";

        for (int axis = 0; axis < 3; ++axis) {
            Bln32 connected = 0;
            if (ECC_getStatusConnected(handle, axis, &connected) == 0 && connected) {
                Int32 pos = 0;
                char actor_name[20] = {0};
                ECC_actorType actor_type;
                
                std::cout << "  Axis " << axis << ": ";
                
                if (ECC_getPosition(handle, axis, &pos) == 0) {
                    std::cout << pos;
                    
                    // Get actor info for units
                    if (ECC_getActorType(handle, axis, &actor_type) == 0) {
                        switch (actor_type) {
                            case ECC_actorLinear:
                                std::cout << " nm [Linear]";
                                break;
                            case ECC_actorGonio:
                                std::cout << " µ° [Goniometer]";
                                break;
                            case ECC_actorRot:
                                std::cout << " µ° [Rotator]";
                                break;
                        }
                    }
                    
                    if (ECC_getActorName(handle, axis, actor_name) == 0) {
                        std::cout << " (" << actor_name << ")";
                    }
                    
                    // Status indicators
                    Bln32 ref_valid = 0;
                    if (ECC_getStatusReference(handle, axis, &ref_valid) == 0 && ref_valid) {
                        std::cout << " [REF]";
                    }
                    
                    Int32 moving = 0;
                    if (ECC_getStatusMoving(handle, axis, &moving) == 0 && moving != 0) {
                        std::cout << " [MOVING]";
                    }
                    
                    std::cout << "\n";
                } else {
                    std::cout << "[Position read failed]\n";
                }
                
                // Show axis configuration for connected axes
                show_axis_config(handle, axis);
                
            } else {
                std::cout << "  Axis " << axis << ": [Not connected]\n";
            }
        }
        std::cout << "\n";

        ECC_Close(handle);
    }

    ECC_ReleaseInfo();
}

void show_axis_config(Int32 handle, int axis) {
    // Get amplitude and frequency
    Int32 amplitude = 0, frequency = 0;
    if (ECC_controlAmplitude(handle, axis, &amplitude, 0) == 0) {
        std::cout << "    Amplitude: " << amplitude << " mV\n";
    }
    if (ECC_controlFrequency(handle, axis, &frequency, 0) == 0) {
        std::cout << "    Frequency: " << frequency << " mHz\n";
    }
    
    // Get target range
    Int32 target_range = 0;
    if (ECC_controlTargetRange(handle, axis, &target_range, 0) == 0) {
        std::cout << "    Target range: " << target_range << " nm/µ°\n";
    }
    
    // Check reference status
    Bln32 ref_valid = 0;
    if (ECC_getStatusReference(handle, axis, &ref_valid) == 0) {
        std::cout << "    Reference valid: " << (ref_valid ? "Yes" : "No") << "\n";
        if (ref_valid) {
            Int32 ref_pos = 0;
            if (ECC_getReferencePosition(handle, axis, &ref_pos) == 0) {
                std::cout << "    Reference position: " << ref_pos << "\n";
            }
        }
    }
    
    // Check EOT status
    Bln32 eot_fwd = 0, eot_bkwd = 0;
    if (ECC_getStatusEotFwd(handle, axis, &eot_fwd) == 0) {
        std::cout << "    EOT Forward: " << (eot_fwd ? "Detected" : "Clear") << "\n";
    }
    if (ECC_getStatusEotBkwd(handle, axis, &eot_bkwd) == 0) {
        std::cout << "    EOT Backward: " << (eot_bkwd ? "Detected" : "Clear") << "\n";
    }
}

void calibrate_axis(int stage_index, int axis) {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 handle;
    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller.\n";
        ECC_ReleaseInfo();
        return;
    }

    std::cout << "Calibrating axis " << axis << "...\n";
    
    // Reset position to establish new reference
    if (ECC_setReset(handle, axis) != 0) {
        std::cerr << "Failed to reset position.\n";
        ECC_Close(handle);
        ECC_ReleaseInfo();
        return;
    }
    
    std::cout << "Position reset. New reference established.\n";
    
    // Wait a moment for the reset to take effect
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Verify calibration
    Int32 position = 0;
    if (ECC_getPosition(handle, axis, &position) == 0) {
        std::cout << "Current position after calibration: " << position << "\n";
    }
    
    Bln32 ref_valid = 0;
    if (ECC_getStatusReference(handle, axis, &ref_valid) == 0) {
        std::cout << "Reference valid: " << (ref_valid ? "Yes" : "No") << "\n";
    }

    ECC_Close(handle);
    ECC_ReleaseInfo();
}

void stop_movement(int stage_index, int axis) {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 handle;
    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller.\n";
        ECC_ReleaseInfo();
        return;
    }

    std::cout << "Stopping movement on axis " << axis << "...\n";
    
    // Disable move control (like pressing Move button again in DAISY)
    Bln32 move_disable = 0;
    if (ECC_controlMove(handle, axis, &move_disable, 1) == 0) {
        std::cout << "✓ Closed-loop control disabled\n";
    } else {
        std::cerr << "✗ Failed to disable movement\n";
    }
    
    // Optionally disable output as well (uncomment if desired)
    // Bln32 output_disable = 0;
    // ECC_controlOutput(handle, axis, &output_disable, 1);
    // std::cout << "✓ Output disabled\n";

    // Check final status
    Int32 moving_status = 0;
    if (ECC_getStatusMoving(handle, axis, &moving_status) == 0) {
        std::cout << "Final status: ";
        switch (moving_status) {
            case 0: std::cout << "IDLE"; break;
            case 1: std::cout << "MOVING"; break;
            case 2: std::cout << "PENDING"; break;
            default: std::cout << "UNKNOWN(" << moving_status << ")"; break;
        }
        std::cout << "\n";
    }

    ECC_Close(handle);
    ECC_ReleaseInfo();
}

void continuous_move(int stage_index, int axis, bool forward, int duration_ms) {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 handle;
    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller.\n";
        ECC_ReleaseInfo();
        return;
    }

    // Enable output
    Bln32 enable = 1;
    ECC_controlOutput(handle, axis, &enable, 1);

    std::cout << "Starting continuous movement " << (forward ? "forward" : "backward") 
              << " for " << duration_ms << "ms...\n";

    // Start continuous movement
    if (forward) {
        ECC_controlContinousFwd(handle, axis, &enable, 1);
    } else {
        ECC_controlContinousBkwd(handle, axis, &enable, 1);
    }

    // Monitor position during movement
    auto start_time = std::chrono::steady_clock::now();
    Int32 start_pos = 0, current_pos = 0;
    ECC_getPosition(handle, axis, &start_pos);

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start_time).count() < duration_ms) {
        
        if (ECC_getPosition(handle, axis, &current_pos) == 0) {
            std::cout << "\rPosition: " << current_pos 
                      << " (Δ: " << (current_pos - start_pos) << ")" << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Stop movement
    enable = 0;
    if (forward) {
        ECC_controlContinousFwd(handle, axis, &enable, 1);
    } else {
        ECC_controlContinousBkwd(handle, axis, &enable, 1);
    }

    std::cout << "\nMovement stopped. Final position: " << current_pos 
              << " (Total movement: " << (current_pos - start_pos) << ")\n";

    // Disable output
    enable = 0;
    ECC_controlOutput(handle, axis, &enable, 1);

    ECC_Close(handle);
    ECC_ReleaseInfo();
}

void single_step_move(int stage_index, int axis, bool backward, int steps) {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 handle;
    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 start_pos = 0, current_pos = 0;
    ECC_getPosition(handle, axis, &start_pos);
    
    std::cout << "Performing " << steps << " step(s) " 
              << (backward ? "backward" : "forward") << "...\n";
    std::cout << "Starting position: " << start_pos << "\n";

    for (int i = 0; i < steps; ++i) {
        if (ECC_setSingleStep(handle, axis, backward) != 0) {
            std::cerr << "Failed to execute step " << (i + 1) << "\n";
            break;
        }
        
        // Wait for step to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (ECC_getPosition(handle, axis, &current_pos) == 0) {
            std::cout << "Step " << (i + 1) << ": Position = " << current_pos 
                      << " (Δ: " << (current_pos - start_pos) << ")\n";
        }
    }

    ECC_Close(handle);
    ECC_ReleaseInfo();
}

void monitor_position(int stage_index, int axis, int duration_seconds) {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 handle;
    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller.\n";
        ECC_ReleaseInfo();
        return;
    }

    std::cout << "Monitoring axis " << axis << " for " << duration_seconds << " seconds...\n";
    std::cout << "Press Ctrl+C to stop early.\n\n";

    auto start_time = std::chrono::steady_clock::now();
    std::vector<Int32> positions;
    
    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - start_time).count() < duration_seconds) {
        
        Int32 position = 0;
        Int32 moving_status = 0;
        Bln32 in_target = 0;
        
        if (ECC_getPosition(handle, axis, &position) == 0) {
            positions.push_back(position);
            
            ECC_getStatusMoving(handle, axis, &moving_status);
            ECC_getStatusTargetRange(handle, axis, &in_target);
            
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            std::cout << "[" << std::setw(3) << elapsed << "s] Position: " 
                      << std::setw(10) << position;
            
            if (moving_status != 0) std::cout << " [MOVING]";
            if (in_target) std::cout << " [TARGET]";
            
            std::cout << "\n";
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Calculate statistics
    if (!positions.empty()) {
        auto minmax = std::minmax_element(positions.begin(), positions.end());
        Int32 range = *minmax.second - *minmax.first;
        
        std::cout << "\nPosition Statistics:\n";
        std::cout << "  Samples: " << positions.size() << "\n";
        std::cout << "  Min: " << *minmax.first << "\n";
        std::cout << "  Max: " << *minmax.second << "\n";
        std::cout << "  Range: " << range << "\n";
    }

    ECC_Close(handle);
    ECC_ReleaseInfo();
}

void set_axis_parameters(int stage_index, int axis, int amplitude, int frequency) {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 handle;
    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller.\n";
        ECC_ReleaseInfo();
        return;
    }

    std::cout << "Configuring axis " << axis << " parameters...\n";

    if (amplitude > 0) {
        if (ECC_controlAmplitude(handle, axis, &amplitude, 1) == 0) {
            std::cout << "✓ Amplitude set to " << amplitude << " mV\n";
        } else {
            std::cerr << "✗ Failed to set amplitude\n";
        }
    }

    if (frequency > 0) {
        if (ECC_controlFrequency(handle, axis, &frequency, 1) == 0) {
            std::cout << "✓ Frequency set to " << frequency << " mHz\n";
        } else {
            std::cerr << "✗ Failed to set frequency\n";
        }
    }

    // Display current configuration - using the modified version for consistency
    std::cout << "\nAxis " << axis << " Configuration:\n";
    
    // Get amplitude and frequency
    Int32 current_amplitude = 0, current_frequency = 0;
    if (ECC_controlAmplitude(handle, axis, &current_amplitude, 0) == 0) {
        std::cout << "  Amplitude: " << current_amplitude << " mV\n";
    }
    if (ECC_controlFrequency(handle, axis, &current_frequency, 0) == 0) {
        std::cout << "  Frequency: " << current_frequency << " mHz\n";
    }
    
    // Get target range
    Int32 target_range = 0;
    if (ECC_controlTargetRange(handle, axis, &target_range, 0) == 0) {
        std::cout << "  Target range: " << target_range << " nm/µ°\n";
    }
    
    // Check reference status
    Bln32 ref_valid = 0;
    if (ECC_getStatusReference(handle, axis, &ref_valid) == 0) {
        std::cout << "  Reference valid: " << (ref_valid ? "Yes" : "No") << "\n";
        if (ref_valid) {
            Int32 ref_pos = 0;
            if (ECC_getReferencePosition(handle, axis, &ref_pos) == 0) {
                std::cout << "  Reference position: " << ref_pos << "\n";
            }
        }
    }
    
    // Check EOT status
    Bln32 eot_fwd = 0, eot_bkwd = 0;
    if (ECC_getStatusEotFwd(handle, axis, &eot_fwd) == 0) {
        std::cout << "  EOT Forward: " << (eot_fwd ? "Detected" : "Clear") << "\n";
    }
    if (ECC_getStatusEotBkwd(handle, axis, &eot_bkwd) == 0) {
        std::cout << "  EOT Backward: " << (eot_bkwd ? "Detected" : "Clear") << "\n";
    }
    
    std::cout << "\n";

    ECC_Close(handle);
    ECC_ReleaseInfo();
}

void save_configuration(int stage_index) {
    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 handle;
    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller.\n";
        ECC_ReleaseInfo();
        return;
    }

    std::cout << "Saving configuration to flash...\n";
    
    if (ECC_setSaveParams(handle) == 0) {
        std::cout << "✓ Configuration saved successfully\n";
        
        // Wait for flash write to complete
        Bln32 writing = 1;
        int timeout = 50; // 5 seconds timeout
        
        while (writing && timeout > 0) {
            if (ECC_getStatusFlash(handle, &writing) == 0) {
                if (!writing) {
                    std::cout << "✓ Flash write completed\n";
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            timeout--;
        }
        
        if (timeout == 0) {
            std::cout << "⚠ Flash write timeout - configuration may not be fully saved\n";
        }
    } else {
        std::cerr << "✗ Failed to save configuration\n";
    }

    ECC_Close(handle);
    ECC_ReleaseInfo();
}

// Modified move_axis function - removed show_axis_config call
void move_axis(int stage_index, int axis, int target_position) {
    if (axis < 0 || axis > 2) {
        std::cerr << "Axis must be 0, 1, or 2.\n";
        return;
    }

    // Declare variables that will be used throughout the function
    Bln32 error_status = 0, eot_fwd = 0, eot_bkwd = 0;

    struct EccInfo* info = nullptr;
    int num_controllers = ECC_Check(&info);
    if (num_controllers <= 0 || stage_index >= num_controllers) {
        std::cerr << "Invalid stage index or no controllers found.\n";
        ECC_ReleaseInfo();
        return;
    }

    Int32 id, handle;
    Bln32 locked;
    if (ECC_getDeviceInfo(stage_index, &id, &locked) != 0) {
        std::cerr << "Failed to get device info for controller " << stage_index << "\n";
        ECC_ReleaseInfo();
        return;
    }

    if (locked) {
        std::cerr << "Controller " << stage_index << " is locked by another application.\n";
        ECC_ReleaseInfo();
        return;
    }

    if (ECC_Connect(stage_index, &handle) != 0) {
        std::cerr << "Failed to connect to controller " << stage_index << "\n";
        ECC_ReleaseInfo();
        return;
    }

    std::cout << "Connected to controller " << stage_index << " (ID=" << id << ")\n";

    // Check if axis is connected
    Bln32 connected = 0;
    if (ECC_getStatusConnected(handle, axis, &connected) != 0 || !connected) {
        std::cerr << "Axis " << axis << " is not connected.\n";
        ECC_Close(handle);
        ECC_ReleaseInfo();
        return;
    }

    // Debug: Check all current settings before movement
    std::cout << "\n=== Pre-Movement Debug Info ===\n";
    
    // Check trigger settings that might interfere
    Bln32 ext_trigger = 0, aquadb_in = 0;
    if (ECC_controlExtTrigger(handle, axis, &ext_trigger, 0) == 0) {
        std::cout << "External trigger: " << (ext_trigger ? "Enabled" : "Disabled") << "\n";
    }
    if (ECC_controlAQuadBIn(handle, axis, &aquadb_in, 0) == 0) {
        std::cout << "AQuadB input: " << (aquadb_in ? "Enabled" : "Disabled") << "\n";
    }
    
    // Disable any external triggers that might interfere
    Bln32 disable = 0;
    ECC_controlExtTrigger(handle, axis, &disable, 1);
    ECC_controlAQuadBIn(handle, axis, &disable, 1);
    std::cout << "Disabled external triggers\n";
    
    // Check current amplitude and frequency
    Int32 current_amp = 0, current_freq = 0;
    ECC_controlAmplitude(handle, axis, &current_amp, 0);
    ECC_controlFrequency(handle, axis, &current_freq, 0);
    std::cout << "Current amplitude: " << current_amp << " mV\n";
    std::cout << "Current frequency: " << current_freq << " mHz\n";

    // Get current position
    Int32 current_pos = 0;
    if (ECC_getPosition(handle, axis, &current_pos) == 0) {
        std::cout << "Current position: " << current_pos << "\n";
    }

    // Set a reasonable target range (10% of movement distance or minimum 1000)
    Int32 movement_distance = std::abs(target_position - current_pos);
    Int32 suggested_range = std::max(1000, movement_distance / 10);
    
    std::cout << "Setting target range to: " << suggested_range << "\n";
    if (ECC_controlTargetRange(handle, axis, &suggested_range, 1) != 0) {
        std::cerr << "Warning: Failed to set target range\n";
    }

    // Enable output stage
    Bln32 enable = 1;
    if (ECC_controlOutput(handle, axis, &enable, 1) != 0) {
        std::cerr << "Failed to enable output for axis " << axis << "\n";
        ECC_Close(handle);
        ECC_ReleaseInfo();
        return;
    }

    // Set target position
    std::cout << "Setting target position to: " << target_position << "\n";
    if (ECC_controlTargetPosition(handle, axis, &target_position, 1) != 0) {
        std::cerr << "Failed to set target position " << target_position << " for axis " << axis << "\n";
        ECC_Close(handle);
        ECC_ReleaseInfo();
        return;
    }

    // Verify target position was set
    Int32 verify_target = 0;
    if (ECC_controlTargetPosition(handle, axis, &verify_target, 0) == 0) {
        std::cout << "Target position verified: " << verify_target << "\n";
    }

    // Check output status before enabling movement
    Bln32 output_status = 0;
    if (ECC_controlOutput(handle, axis, &output_status, 0) == 0) {
        std::cout << "Output status before move: " << (output_status ? "Enabled" : "Disabled") << "\n";
    }

    // Start the movement
    std::cout << "Enabling movement...\n";
    Bln32 move_enable = 1;
    if (ECC_controlMove(handle, axis, &move_enable, 1) != 0) {
        std::cerr << "Failed to start movement for axis " << axis << "\n";
        ECC_Close(handle);
        ECC_ReleaseInfo();
        return;
    }

    // Check status immediately after starting movement
    std::cout << "\n=== Immediate Post-Movement-Enable Status ===\n";
    Int32 immediate_status = 0;
    if (ECC_getStatusMoving(handle, axis, &immediate_status) == 0) {
        std::cout << "Moving status immediately after enable: ";
        switch (immediate_status) {
            case 0: std::cout << "IDLE"; break;
            case 1: std::cout << "MOVING"; break;
            case 2: std::cout << "PENDING"; break;
            default: std::cout << "UNKNOWN(" << immediate_status << ")"; break;
        }
        std::cout << "\n";
    }

    // Check for any error conditions immediately
    ECC_getStatusError(handle, axis, &error_status);
    ECC_getStatusEotFwd(handle, axis, &eot_fwd);  
    ECC_getStatusEotBkwd(handle, axis, &eot_bkwd);
    
    std::cout << "Error status: " << (error_status ? "ERROR" : "OK") << "\n";
    std::cout << "EOT Forward: " << (eot_fwd ? "DETECTED" : "Clear") << "\n";
    std::cout << "EOT Backward: " << (eot_bkwd ? "DETECTED" : "Clear") << "\n";
    
    // Wait a moment and check again
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    Int32 status_after_wait = 0;
    if (ECC_getStatusMoving(handle, axis, &status_after_wait) == 0) {
        std::cout << "Moving status after 100ms wait: ";
        switch (status_after_wait) {
            case 0: std::cout << "IDLE"; break;
            case 1: std::cout << "MOVING"; break;
            case 2: std::cout << "PENDING"; break;
            default: std::cout << "UNKNOWN(" << status_after_wait << ")"; break;
        }
        std::cout << "\n";
    }
    
    // Check if position changed at all
    Int32 pos_after_wait = 0;
    if (ECC_getPosition(handle, axis, &pos_after_wait) == 0) {
        std::cout << "Position after 100ms: " << pos_after_wait << " (change: " << (pos_after_wait - current_pos) << ")\n";
    }
    
    std::cout << "=== Starting Movement Monitoring ===\n";

    std::cout << "\nMovement Progress:\n";
    std::cout << "Moving from " << current_pos << " to " << target_position << "\n";

    // Wait for movement to complete with better monitoring
    Int32 moving_status = 1;
    int timeout_count = 0;
    const int max_timeout = 300; // 30 seconds timeout
    Int32 last_pos = current_pos;
    int stuck_count = 0;
    
    while (moving_status != 0 && timeout_count < max_timeout) {
        if (ECC_getStatusMoving(handle, axis, &moving_status) != 0) {
            std::cout << "Failed to get movement status.\n";
            break;
        }
        
        // Get current position during movement
        Int32 current_pos_during_move = 0;
        if (ECC_getPosition(handle, axis, &current_pos_during_move) == 0) {
            Int32 remaining = target_position - current_pos_during_move;
            double progress = (movement_distance > 0) ? 
                (1.0 - (double)std::abs(remaining) / movement_distance) * 100.0 : 100.0;
            
            std::cout << "Position: " << current_pos_during_move 
                      << " → " << target_position 
                      << " (" << std::fixed << std::setprecision(1) << progress << "%)";
            
            switch (moving_status) {
                case 0: std::cout << " [IDLE]"; break;
                case 1: std::cout << " [MOVING]"; break;
                case 2: std::cout << " [PENDING]"; break;
                default: std::cout << " [UNKNOWN:" << moving_status << "]"; break;
            }
            std::cout << "\n";
            
            // Check if movement is stuck
            if (std::abs(current_pos_during_move - last_pos) < 10) {
                stuck_count++;
                if (stuck_count > 20) { // 2 seconds without movement
                    std::cout << "Movement appears stuck, checking status...\n";
                    
                    // Check for EOT
                    Bln32 eot_fwd = 0, eot_bkwd = 0;
                    ECC_getStatusEotFwd(handle, axis, &eot_fwd);
                    ECC_getStatusEotBkwd(handle, axis, &eot_bkwd);
                    if (eot_fwd || eot_bkwd) {
                        std::cout << "End of travel detected. Movement stopped.\n";
                        break;
                    }
                    
                    // Check for errors
                    Bln32 error_status = 0;
                    if (ECC_getStatusError(handle, axis, &error_status) == 0 && error_status) {
                        std::cout << "Error detected on axis. Movement stopped.\n";
                        break;
                    }
                }
            } else {
                stuck_count = 0;
            }
            last_pos = current_pos_during_move;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        timeout_count++;
    }

    if (timeout_count >= max_timeout) {
        std::cout << "Movement timeout reached.\n";
    }

    // Stop movement (disable closed-loop control like DAISY's Move button toggle)
    move_enable = 0;
    ECC_controlMove(handle, axis, &move_enable, 1);
    std::cout << "\nClosed-loop control disabled (movement stopped)\n";

    // Get final position
    Int32 final_pos = 0;
    if (ECC_getPosition(handle, axis, &final_pos) == 0) {
        std::cout << "\nMovement Results:\n";
        std::cout << "  Final position: " << final_pos << "\n";
        std::cout << "  Target position: " << target_position << "\n";
        std::cout << "  Position difference: " << (final_pos - target_position) << "\n";
        std::cout << "  Movement distance: " << (final_pos - current_pos) << "\n";
    }

    // Check if we're in target range
    Bln32 in_target_range = 0;
    if (ECC_getStatusTargetRange(handle, axis, &in_target_range) == 0) {
        if (in_target_range) {
            std::cout << "✓ Target reached successfully!\n";
        } else {
            std::cout << "✗ Target not reached (outside target range).\n";
            
            // Show current target range
            Int32 current_range = 0;
            if (ECC_controlTargetRange(handle, axis, &current_range, 0) == 0) {
                std::cout << "  Current target range: ±" << current_range << "\n";
            }
        }
    }

    // Check for any errors or EOT
    if (ECC_getStatusError(handle, axis, &error_status) == 0 && error_status) {
        std::cout << "⚠ Warning: Error status detected on axis " << axis << "\n";
    }
    if (ECC_getStatusEotFwd(handle, axis, &eot_fwd) == 0 && eot_fwd) {
        std::cout << "⚠ Forward end of travel detected\n";
    }
    if (ECC_getStatusEotBkwd(handle, axis, &eot_bkwd) == 0 && eot_bkwd) {
        std::cout << "⚠ Backward end of travel detected\n";
    }

    // Disable output
    enable = 0;
    ECC_controlOutput(handle, axis, &enable, 1);

    ECC_Close(handle);
    ECC_ReleaseInfo();
}
