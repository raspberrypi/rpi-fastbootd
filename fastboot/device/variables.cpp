/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "variables.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <inttypes.h>
#include <stdio.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
// Network interface access via getifaddrs() (POSIX libc)
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

// Kernel log access via klogctl() (Linux syscall)
#include <sys/klog.h>

#include "fastboot_device.h"
#include "flashing.h"
#include "utility.h"
#include "spawn.h"
#include "wait.h"

#include <rpifwcrypto.h>

#ifdef FB_ENABLE_FETCH
static constexpr bool kEnableFetch = true;
#else
static constexpr bool kEnableFetch = false;
#endif

// using MergeStatus = android::hal::BootControlClient::MergeStatus;
// using aidl::android::hardware::fastboot::FileSystemType;
// using namespace android::fs_mgr;
using namespace std::string_literals;

constexpr char kFastbootProtocolVersion[] = "0.4";

bool GetVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    *message = kFastbootProtocolVersion;
    return true;
}

bool GetBootloaderVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/version", message);
    return true;
}

bool GetBasebandVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                        std::string* message) {
    *message = android::base::GetProperty("ro.build.expect.baseband", "");
    return true;
}

bool GetOsVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    *message = android::base::GetProperty("ro.build.version.release", "");
    return true;
}

bool GetVndkVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                    std::string* message) {
    *message = android::base::GetProperty("ro.vndk.version", "");
    return true;
}

bool GetProduct(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    android::base::ReadFileToString("/sys/firmware/devicetree/base/model", message);
    return true;
}

bool GetSerial(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    std::string serial;
    if (android::base::ReadFileToString("/proc/device-tree/chosen/rpi-serial64", &serial)) {
        *message = serial;
    } else if (android::base::ReadFileToString("/proc/device-tree/serial-number", &serial)) {
        *message = serial;
    }
    return true;
}

bool GetRpiDuid(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                std::string *message)
{
    android::base::ReadFileToString("/proc/device-tree/chosen/rpi-duid", message);
    return true;
}

namespace {
    void inspectOtp(std::function<bool(std::string *, std::string *)> inspectorfn, std::string *message) {
        if (inspectorfn == nullptr) {
            return;
        }

        std::string otp_dump;
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/otp.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);
    
        char *arg[] = {"/usr/bin/vcgencmd", "otp_dump", NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/usr/bin/vcgencmd", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);
    
        if (ret)
        {
            *message = strerror(errno);
            return;
        }
        else if (subprocess_rc)
        {
            *message = "vcgencmd failed";
            return;
        }
        else
        {
            android::base::ReadFileToString("/tmp/otp.log", &otp_dump);
        }

        std::istringstream stream(otp_dump);
        std::string line;
        while (std::getline(stream, line)) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
    
                if (inspectorfn(&key, &value)) {
                    return;
                }
            }
        }
    }
} // namespace anonymous

namespace {
    // Helper to extract bit field from any OTP register
    bool GetOtpRegisterBitField(const std::string& register_key, uint32_t mask, int shift, 
                                std::string* message) {
        bool found = false;
        inspectOtp([&](std::string *key, std::string *value) {
            if (*key == register_key) {
            uint32_t value_int = std::stoul(*value, 0, 16);
                *message = android::base::StringPrintf("0x%X", (value_int & mask) >> shift);
                found = true;
            return true;
        }
        return false;
        }, message);
        return found;
    }
} // namespace

bool GetRevisionProcessor(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                          std::string *message)
{
    GetOtpRegisterBitField("32", 0xF000, 12, message);
    return true;
}

bool GetRevisionManufacturer(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                             std::string *message)
{
    GetOtpRegisterBitField("32", 0xF0000, 16, message);
    return true;
}

bool GetRevisionMemory(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                       std::string *message)
{
    GetOtpRegisterBitField("32", 0x700000, 20, message);
            return true;
        }

bool GetRevisionType(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                     std::string *message)
{
    GetOtpRegisterBitField("32", 0x0FF0, 4, message);
    return true;
}

bool GetRevisionRevision(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                       std::string *message)
{
    GetOtpRegisterBitField("32", 0x0F, 0, message);
    return true;
}

namespace {
    // Helper to format MAC address hex string with colons
    std::string FormatMacPart(const std::string& hex) {
        std::string result = hex;
        for (size_t i = 2; i < result.length(); i += 3) {
            result.insert(i, ":");
        }
        return result;
    }

    // Helper to extract MAC address from OTP registers
    bool GetMacFromOtp(const std::string& low_key, const std::string& high_key, std::string* message) {
        std::string mac_lo, mac_hi;
        uint required = 2;
        inspectOtp([&](std::string *key, std::string *value) {
            if (*key == low_key) {
                mac_lo = FormatMacPart(value->substr(0, 4));
                required--;
            } else if (*key == high_key) {
                mac_hi = FormatMacPart(*value);
                required--;
            }
            return !required;  // Return true when both found
        }, message);
        *message = mac_hi + ":" + mac_lo;
        return true;
    }

    // Helper to execute a command and capture output to a file
    bool ExecuteCommandToFile(const char* command_path, char* const args[], 
                              const char* output_file, std::string* output) {
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, output_file, O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, command_path, args, NULL, &action);
        posix_spawn_file_actions_destroy(&action);

        if (ret == 0 && subprocess_rc == 0) {
            if (output) {
                android::base::ReadFileToString(output_file, output);
            }
            return true;
        }
        return false;
    }

    // Helper to try executing a command with multiple possible paths
    bool TryExecuteCommand(const std::vector<const char*>& command_paths, 
                          const std::vector<const char*>& args,
                          const char* output_file, 
                          std::string* output) {
        for (const char* cmd_path : command_paths) {
            if (access(cmd_path, X_OK) != 0) {
                continue;  // Path doesn't exist or not executable
            }
            
            // Build args array with command path as first element
            std::vector<char*> full_args;
            full_args.push_back(const_cast<char*>(cmd_path));
            for (const char* arg : args) {
                full_args.push_back(const_cast<char*>(arg));
            }
            full_args.push_back(NULL);
            
            if (ExecuteCommandToFile(cmd_path, full_args.data(), output_file, output)) {
    return true;
            }
        }
        return false;
    }

    // Helper for trying multiple network interfaces
    const char* kNetworkInterfaces[] = {"end0", "eth0"};
    constexpr size_t kNumNetworkInterfaces = 2;

    // IP version for protocol-agnostic functions
    enum class IpVersion { V4, V6, BOTH };

    // Native network interface info retrieval using getifaddrs()
    // Returns true if interface found with matching IP version
    bool GetInterfaceInfoNative(const char* interface_name, IpVersion version,
                                std::string* address, std::string* netmask) {
        struct ifaddrs *ifaddr, *ifa;
        
        if (getifaddrs(&ifaddr) == -1) {
            LOG(WARNING) << "getifaddrs() failed: " << strerror(errno);
            return false;
        }
        
        bool found = false;
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (strcmp(ifa->ifa_name, interface_name) != 0) continue;
            
            int family = ifa->ifa_addr->sa_family;
            
            // Check if this is the right IP version
            if ((version == IpVersion::V4 && family == AF_INET) ||
                (version == IpVersion::V6 && family == AF_INET6)) {
                
                char addr_buf[INET6_ADDRSTRLEN];
                
                // Get address
                void* addr_ptr = (family == AF_INET) 
                    ? (void*)&((struct sockaddr_in*)ifa->ifa_addr)->sin_addr
                    : (void*)&((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr;
                
                if (inet_ntop(family, addr_ptr, addr_buf, sizeof(addr_buf)) == NULL) {
                    continue;
                }
                
                // Skip link-local IPv6 addresses
                if (version == IpVersion::V6 && strncmp(addr_buf, "fe80:", 5) == 0) {
                    continue;
                }
                
                if (address) {
                    *address = addr_buf;
                }
                
                // Get netmask if requested
                if (netmask && ifa->ifa_netmask) {
                    void* mask_ptr = (family == AF_INET)
                        ? (void*)&((struct sockaddr_in*)ifa->ifa_netmask)->sin_addr
                        : (void*)&((struct sockaddr_in6*)ifa->ifa_netmask)->sin6_addr;
                    
                    if (family == AF_INET) {
                        // For IPv4, convert to dotted decimal
                        char mask_buf[INET_ADDRSTRLEN];
                        if (inet_ntop(family, mask_ptr, mask_buf, sizeof(mask_buf))) {
                            *netmask = mask_buf;
                        }
                    } else {
                        // For IPv6, calculate prefix length from netmask
                        struct in6_addr* mask6 = (struct in6_addr*)mask_ptr;
                        int prefix_len = 0;
                        for (int i = 0; i < 16; i++) {
                            unsigned char byte = mask6->s6_addr[i];
                            for (int bit = 7; bit >= 0; bit--) {
                                if (byte & (1 << bit)) {
                                    prefix_len++;
                                } else {
                                    // Once we hit a zero bit, stop counting
                                    goto done_counting;
                                }
                            }
                        }
                    done_counting:
                        *netmask = std::to_string(prefix_len);
                    }
                }
                
                found = true;
                break;
            }
        }
        
        freeifaddrs(ifaddr);
        return found;
    }

    bool GetInterfaceAddressNative(const char* interface_name, IpVersion version,
                                   std::string* address) {
        return GetInterfaceInfoNative(interface_name, version, address, nullptr);
    }

    bool GetInterfaceNetmaskNative(const char* interface_name, IpVersion version,
                                   std::string* netmask) {
        return GetInterfaceInfoNative(interface_name, version, nullptr, netmask);
    }

    // Parse IPv4 gateway from /proc/net/route (hex little-endian format)
    bool GetIpv4GatewayNative(const char* interface_name, std::string* gateway) {
        std::ifstream route_file("/proc/net/route");
        if (!route_file.is_open()) {
            LOG(WARNING) << "Failed to open /proc/net/route: " << strerror(errno);
            return false;
        }
        
        std::string line;
        // Skip header line
        std::getline(route_file, line);
        
        while (std::getline(route_file, line)) {
            std::istringstream iss(line);
            std::string iface, dest_hex, gateway_hex, flags_hex;
            
            iss >> iface >> dest_hex >> gateway_hex >> flags_hex;
            
            // Check if this is the default route (destination = 00000000)
            // and matches our interface
            if (dest_hex == "00000000" && iface == interface_name) {
                // Parse gateway from hex (little-endian)
                if (gateway_hex == "00000000") {
                    continue;  // No gateway (0.0.0.0)
                }
                
                // Convert hex to IP address (little-endian)
                unsigned long gw = std::stoul(gateway_hex, nullptr, 16);
                *gateway = android::base::StringPrintf("%lu.%lu.%lu.%lu",
                    (gw & 0xFF),
                    ((gw >> 8) & 0xFF),
                    ((gw >> 16) & 0xFF),
                    ((gw >> 24) & 0xFF));
                
                LOG(INFO) << "Found IPv4 gateway via /proc/net/route: " << *gateway;
            return true;
        }
        }
        
        return false;
    }
    
    // Parse IPv6 gateway from /proc/net/ipv6_route
    bool GetIpv6GatewayNative(const char* interface_name, std::string* gateway) {
        std::ifstream route_file("/proc/net/ipv6_route");
        if (!route_file.is_open()) {
            LOG(WARNING) << "Failed to open /proc/net/ipv6_route: " << strerror(errno);
            return false;
        }
        
        std::string line;
        while (std::getline(route_file, line)) {
            std::istringstream iss(line);
            std::string dest, dest_prefix, src, src_prefix, next_hop, metric, refcnt, use, flags, iface;
            
            // Format: dest destprefix src srcprefix nexthop metric refcnt use flags iface
            iss >> dest >> dest_prefix >> src >> src_prefix >> next_hop >> metric >> refcnt >> use >> flags >> iface;
            
            // Check if this is a default route (dest = all zeros, prefix = 00)
            // and matches our interface
            if (dest == "00000000000000000000000000000000" && dest_prefix == "00" && iface == interface_name) {
                // Check if there's actually a gateway (next_hop != all zeros)
                if (next_hop == "00000000000000000000000000000000") {
                    continue;  // No gateway
                }
                
                // Parse next_hop (32 hex chars = 16 bytes = 128 bits)
                // Format IPv6 address from hex string
                if (next_hop.length() != 32) {
                    continue;
                }
                
                // Convert to standard IPv6 notation (8 groups of 4 hex digits)
                std::string ipv6;
                for (size_t i = 0; i < 32; i += 4) {
                    if (i > 0) ipv6 += ":";
                    ipv6 += next_hop.substr(i, 4);
                }
                
                // Simplify IPv6 address (remove leading zeros)
                *gateway = ipv6;
                
                LOG(INFO) << "Found IPv6 gateway via /proc/net/ipv6_route: " << *gateway;
    return true;
            }
        }
        
        return false;
    }

    // Read kernel log buffer using klogctl() syscall
    bool ReadKernelLogNative(FastbootDevice* device) {
        int buffer_size = klogctl(10, nullptr, 0);  // Get buffer size
        if (buffer_size < 0) {
            LOG(WARNING) << "klogctl(SIZE_BUFFER) failed: " << strerror(errno);
            return false;
        }

        std::vector<char> buffer(buffer_size + 1);
        
        int bytes_read = klogctl(3, buffer.data(), buffer_size);  // Read all messages
        if (bytes_read < 0) {
            LOG(WARNING) << "klogctl(READ_ALL) failed: " << strerror(errno);
            return false;
        }
        
        buffer[bytes_read] = '\0';
        
        std::istringstream stream(buffer.data());
        std::string line;
        while (std::getline(stream, line)) {
            if (!device->WriteInfo(line)) {
                LOG(ERROR) << "Failed to write kernel log line to device";
                return false;
            }
        }
        
        LOG(INFO) << "Read kernel log via klogctl - " << bytes_read << " bytes";
        return true;
    }

    // Check if address matches IP version
    bool IsIpVersion(const std::string& addr, IpVersion version) {
        bool has_colon = addr.find(':') != std::string::npos;
        bool has_dot = addr.find('.') != std::string::npos;
        
        switch (version) {
            case IpVersion::V4:
                return has_dot && !has_colon;
            case IpVersion::V6:
                return has_colon;
            case IpVersion::BOTH:
            return true;
        }
        return false;
    }

    // Parse IPv4 address from ifconfig or ip command output
    bool ParseIpv4Address(const std::string& output, std::string* address) {
        std::istringstream stream(output);
        std::string line;
        
        // Try to parse ifconfig output first
        while (std::getline(stream, line)) {
            size_t pos = line.find("inet ");
            if (pos != std::string::npos) {
                std::string inet_part = line.substr(pos + 5);
                std::istringstream iss(inet_part);
                std::string addr;
                
                // Try common format first
                iss >> addr;
                if (addr.find('.') != std::string::npos) {
                    *address = addr;
    return true;
}

                // Try older format with "addr:" prefix
                pos = inet_part.find("addr:");
                if (pos != std::string::npos) {
                    std::string addr_part = inet_part.substr(pos + 5);
                    pos = addr_part.find(' ');
                    if (pos != std::string::npos) {
                        *address = addr_part.substr(0, pos);
                        return true;
                    }
                }
            }
        }
        
        // Try to parse ip command output (format: "inet 192.168.1.2/24 ...")
        stream.clear();
        stream.seekg(0);
        
        while (std::getline(stream, line)) {
            size_t pos = line.find("inet ");
            if (pos != std::string::npos) {
                std::string inet_part = line.substr(pos + 5);
                pos = inet_part.find('/');
                if (pos != std::string::npos) {
                    *address = inet_part.substr(0, pos);
            return true;
                }
            }
        }
        
            return false;
        }

    // Parse IPv6 address from ifconfig or ip command output (skip link-local)
    bool ParseIpv6Address(const std::string& output, std::string* address) {
        std::istringstream stream(output);
        std::string line;
        
        while (std::getline(stream, line)) {
            size_t pos = line.find("inet6 ");
            if (pos != std::string::npos) {
                std::string inet_part = line.substr(pos + 6);
                
                // Skip link-local addresses (fe80::)
                if (inet_part.find("fe80::") != std::string::npos) {
                    continue;
                }
                
                // Try standard format first
                std::istringstream iss(inet_part);
                std::string addr;
                iss >> addr;
                
                // Remove scope ID if present (e.g., "%end0" or "%eth0")
                pos = addr.find('%');
                if (pos != std::string::npos) {
                    addr = addr.substr(0, pos);
                }
                
                // Remove prefix length if present (e.g., "/64")
                pos = addr.find('/');
                if (pos != std::string::npos) {
                    addr = addr.substr(0, pos);
                }
                
                if (addr.find(':') != std::string::npos) {
                    *address = addr;
    return true;
}

                // Try alternative format with "addr:" prefix
                pos = inet_part.find("addr:");
                if (pos != std::string::npos) {
                    std::string addr_part = inet_part.substr(pos + 5);
                    pos = addr_part.find(' ');
                    if (pos != std::string::npos) {
                        addr_part = addr_part.substr(0, pos);
                    }
                    
                    // Remove prefix length if present
                    pos = addr_part.find('/');
                    if (pos != std::string::npos) {
                        addr_part = addr_part.substr(0, pos);
                    }
                    
                    if (addr_part.find(':') != std::string::npos) {
                        *address = addr_part;
                        return true;
                    }
                }
            }
        }
        
        return false;
    }

    // Parse gateway from route command output
    bool ParseGatewayFromRouteOutput(const std::string& output, 
                                      const std::string& interface, 
                                      IpVersion version, 
                                      std::string* gateway) {
        std::istringstream stream(output);
        std::string line;
        
        while (std::getline(stream, line)) {
            // Check for default route with our interface
            bool is_default = (line.find("0.0.0.0") == 0 || 
                              line.find("::/0") != std::string::npos || 
                              line.find("default") == 0);
            
            if (is_default && line.find(interface) != std::string::npos) {
                std::istringstream iss(line);
                std::string dest, gw;
                iss >> dest >> gw;
                
                // Handle "default via GATEWAY" format
                if (dest == "default" && gw == "via") {
                    iss >> gw;
                }
                
                if (IsIpVersion(gw, version)) {
                    *gateway = gw;
            return true;
                }
            }
        }
            return false;
        }

    // Parse gateway from ip route output (format: "default via X.X.X.X dev ...")
    bool ParseGatewayFromIpRouteOutput(const std::string& output, std::string* gateway) {
        std::istringstream stream(output);
        std::string line;
        
        if (std::getline(stream, line)) {
            size_t pos = line.find("via ");
            if (pos != std::string::npos) {
                std::string via_part = line.substr(pos + 4);
                pos = via_part.find(' ');
                if (pos != std::string::npos) {
                    *gateway = via_part.substr(0, pos);
    return true;
                }
            }
        }
        return false;
    }

    // Convert CIDR prefix length to dotted decimal netmask
    std::string CidrToNetmask(int prefix_length) {
        if (prefix_length < 0 || prefix_length > 32) {
            return "";
        }
        uint32_t netmask = prefix_length ? (0xFFFFFFFF << (32 - prefix_length)) : 0;
        return android::base::StringPrintf("%d.%d.%d.%d",
            (netmask >> 24) & 0xFF,
            (netmask >> 16) & 0xFF,
            (netmask >> 8) & 0xFF,
            netmask & 0xFF);
    }

    // Parse netmask from ifconfig output
    bool ParseNetmaskFromIfconfig(const std::string& output, IpVersion version, 
                                  std::string* netmask) {
        std::istringstream stream(output);
        std::string line;
        const char* search_str = (version == IpVersion::V4) ? "inet " : "inet6 ";
        
        while (std::getline(stream, line)) {
            size_t pos = line.find(search_str);
            if (pos == std::string::npos) continue;
            
            // Skip link-local for IPv6
            if (version == IpVersion::V6 && line.find("fe80::") != std::string::npos) {
                continue;
            }
            
            // Look for "netmask" keyword
            pos = line.find("netmask ");
            if (pos != std::string::npos) {
                std::string netmask_part = line.substr(pos + 8);
                std::istringstream iss(netmask_part);
                iss >> *netmask;
                return true;
            }
            
            // Look for older "Mask:" format
            pos = line.find("Mask:");
            if (pos != std::string::npos) {
                *netmask = line.substr(pos + 5);
            return true;
            }
            
            // For IPv6, look for "prefixlen"
            if (version == IpVersion::V6) {
                pos = line.find("prefixlen ");
                if (pos != std::string::npos) {
                    std::string prefix_part = line.substr(pos + 10);
                    std::istringstream iss(prefix_part);
                    iss >> *netmask;
                    return true;
                }
            }
            
            // Also try to find "/prefix" format
            pos = line.find("/");
            if (pos != std::string::npos) {
                std::string after_slash = line.substr(pos + 1);
                pos = after_slash.find(" ");
                if (pos != std::string::npos) {
                    *netmask = after_slash.substr(0, pos);
        } else {
                    *netmask = after_slash;
                }
                return true;
            }
        }
            return false;
        }

    // Parse prefix from ip command output
    bool ParsePrefixFromIpOutput(const std::string& output, IpVersion version, 
                                 std::string* prefix) {
        std::istringstream stream(output);
        std::string line;
        const char* search_str = (version == IpVersion::V4) ? "inet " : "inet6 ";
        
        while (std::getline(stream, line)) {
            size_t pos = line.find(search_str);
            if (pos == std::string::npos) continue;
            
            // Skip link-local for IPv6
            if (version == IpVersion::V6 && line.find("fe80::") != std::string::npos) {
                continue;
            }
            
            std::string inet_part = line.substr(pos + strlen(search_str));
            pos = inet_part.find('/');
            if (pos != std::string::npos) {
                std::string prefix_str = inet_part.substr(pos + 1);
                pos = prefix_str.find(' ');
                if (pos != std::string::npos) {
                    *prefix = prefix_str.substr(0, pos);
                } else {
                    *prefix = prefix_str;
                }
    return true;
            }
        }
        return false;
    }

    // Unified DNS server discovery for IPv4 and IPv6
    bool FindDnsServers(IpVersion version, std::string* result) {
        std::vector<std::string> dns_servers;
        
        // Method 1: Check /etc/resolv.conf
        std::string resolv_conf;
        if (android::base::ReadFileToString("/etc/resolv.conf", &resolv_conf)) {
            std::istringstream stream(resolv_conf);
            std::string line;
            
            while (std::getline(stream, line)) {
                if (line.empty() || line[0] == '#') continue;
                
                size_t pos = line.find("nameserver ");
                if (pos != std::string::npos) {
                    std::string server = line.substr(pos + 11);
                    // Trim whitespace
                    server.erase(0, server.find_first_not_of(" \t"));
                    server.erase(server.find_last_not_of(" \t\r\n") + 1);
                    
                    if (IsIpVersion(server, version)) {
                        dns_servers.push_back(server);
                    }
                }
            }
            
            if (!dns_servers.empty()) {
                *result = android::base::Join(dns_servers, " ");
                return true;
            }
        }
        
        // Method 2: Try nmcli for each interface
        const char* dns_field = (version == IpVersion::V4) ? "IP4.DNS" : "IP6.DNS";
        for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
            std::string output;
            if (TryExecuteCommand({"/usr/bin/nmcli"}, 
                                 {"device", "show", kNetworkInterfaces[i]},
                                 "/tmp/dns-nmcli.log", &output)) {
                std::istringstream stream(output);
                std::string line;
                
                while (std::getline(stream, line)) {
                    if (line.find(dns_field) != std::string::npos) {
                        size_t pos = line.find(":");
                        if (pos != std::string::npos) {
                            std::string server = line.substr(pos + 1);
                            server.erase(0, server.find_first_not_of(" \t"));
                            server.erase(server.find_last_not_of(" \t") + 1);
                            
                            if (!server.empty() && IsIpVersion(server, version)) {
                                dns_servers.push_back(server);
                            }
                        }
                    }
                }
                
                if (!dns_servers.empty()) {
                    *result = android::base::Join(dns_servers, " ");
                    return true;
                }
            }
        }
        
        *result = (version == IpVersion::V4) ? "No IPv4 DNS servers found" 
                                              : "No IPv6 DNS servers found";
        return false;
    }

    // DHCP configuration for v4/v6
    struct DhcpConfig {
        std::string client_port;
        std::string server_port;
        std::vector<std::string> process_names;
        std::vector<std::string> lease_path_templates;
    };

    // Unified DHCP detection for IPv4 and IPv6
    bool DetectDhcp(const char* interface, const DhcpConfig& config, std::string* result) {
        // Method 1: Check for DHCP ports in netstat
        std::string output;
        if (TryExecuteCommand({"/bin/netstat"}, {"-nau"}, "/tmp/dhcp-info.log", &output)) {
            if (output.find(config.client_port) != std::string::npos || 
                (output.find(config.server_port) != std::string::npos && 
                 output.find(interface) != std::string::npos)) {
                *result = "yes";
                return true;
            }
        }
        
        // Method 2: Check for DHCP client processes
        if (TryExecuteCommand({"/bin/ps"}, {"aux"}, "/tmp/dhcp-proc.log", &output)) {
            for (const auto& proc_name : config.process_names) {
                if (output.find(proc_name) != std::string::npos && 
                    output.find(interface) != std::string::npos) {
                    *result = "yes";
                    return true;
                }
            }
        }
        
        // Method 3: Check for DHCP lease files
        for (const auto& lease_template : config.lease_path_templates) {
            char lease_path[PATH_MAX];
            snprintf(lease_path, PATH_MAX, lease_template.c_str(), interface);
            if (access(lease_path, F_OK) == 0) {
                *result = "yes";
                return true;
            }
        }
        
        *result = "no";
        return true;
    }
} // namespace

bool GetMacEthernet(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                    std::string* message) {
    return GetMacFromOtp("50", "51", message);
}

bool GetMacWifi(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                std::string *message)
{
    return GetMacFromOtp("52", "53", message);
}

bool GetMacBt(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
              std::string *message)
{
    return GetMacFromOtp("54", "55", message);
}

bool GetMmcCid(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
               std::string *message)
{
    std::string cid;
    android::base::ReadFileToString("/sys/block/mmcblk0/device/cid", &cid);
    *message = cid;
    return true;
}

bool GetMmcSectorSize(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                      std::string *message)
{
    std::string sector_size;
    android::base::ReadFileToString("/sys/block/mmcblk0/queue/hw_sector_size", &sector_size);
    *message = sector_size;
    return true;
}

bool GetMmcSectorCount(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                       std::string *message)
{
    std::string total_sectors;
    android::base::ReadFileToString("/sys/block/mmcblk0/size", &total_sectors);
    *message = total_sectors;
    return true;
}

bool GetSignedEeprom(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                           std::string* message) {
    std::string signed_raw = {};
    if (android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/signed", &signed_raw)) {
        try {
            *message = (std::stoi(signed_raw) & (1 << 0)) ? "present" : "not present";
        } catch (const std::invalid_argument& e) {
            *message = "not available";
        } catch (const std::out_of_range& e) {
            *message = "not available";
        }
    } else {
        *message = "not available";
    }
    return true;
}

bool GetSignedDevkey(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                         std::string* message) {
    std::string signed_raw = {};
    if (android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/signed", &signed_raw)) {
        try {
            *message = (std::stoi(signed_raw) & (1 << 2)) ? "present" : "not present";
        } catch (const std::invalid_argument& e) {
            *message = "not available";
        } catch (const std::out_of_range& e) {
            *message = "not available";
        }
    } else {
        *message = "not available";
    }
    return true;
}

bool GetSignedOtp(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                        std::string* message) {
    std::string signed_raw = {};
    if (android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/signed", &signed_raw)) {
        try {
            *message = (std::stoi(signed_raw) & (1 << 3)) ? "present" : "not present";
        } catch (const std::invalid_argument& e) {
            *message = "not available";
        } catch (const std::out_of_range& e) {
            *message = "not available";
        }
    } else {
        *message = "not available";
    }
    return true;
}

bool GetSecure(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    pid_t pid;
    char *arg[] = {"/usr/bin/rpi-otp-private-key", "-c", NULL};
    int ret;
    int wstatus;
    ret = posix_spawnp(&pid, "/usr/bin/rpi-otp-private-key", NULL, NULL, arg, NULL);

    if (ret) {
        *message = strerror(ret);
        return false;
    }

    do {
        ret = waitpid(pid, &wstatus, 0);
        if (ret == -1) {
            *message = "waitpid failed";
            return false;
        }
    } while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));

    // Check the exit status of rpi-otp-private-key -c
    // Exit code 0 = key is set (non-zero), return "yes"
    // Exit code 1 = key is all zeros (not set), return "no"
    if (WIFEXITED(wstatus)) {
        int exit_status = WEXITSTATUS(wstatus);
        if (exit_status == 0) {
            *message = "yes";  // Key is set
        } else if (exit_status == 1) {
            *message = "no";   // Key is not set (all zeros)
        } else {
            *message = "error"; // Unexpected exit code
        }
    } else {
        *message = "error";  // Process terminated abnormally
    }
    
    return true;
}

bool GetVariant(FastbootDevice* device, const std::vector<std::string>& /* args */,
                std::string* message) {
    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
        *message = "Fastboot HAL not found";
        return false;
    // }
    // std::string device_variant = "";
    // auto status = fastboot_hal->getVariant(&device_variant);

    // if (!status.isOk()) {
    //     *message = "Unable to get device variant";
    //     LOG(ERROR) << message->c_str() << status.getDescription();
    //     return false;
    // }

    // *message = device_variant;
    // return true;
}

bool GetBatteryVoltageHelper(FastbootDevice* device, int32_t* battery_voltage) {
    // using aidl::android::hardware::health::HealthInfo;

    // auto health_hal = device->health_hal();
    // if (!health_hal) {
        return false;
    // }

    // HealthInfo health_info;
    // auto res = health_hal->getHealthInfo(&health_info);
    // if (!res.isOk()) return false;
    // *battery_voltage = health_info.batteryVoltageMillivolts;
    // return true;
}

bool GetBatterySoCOk(FastbootDevice* device, const std::vector<std::string>& /* args */,
                     std::string* message) {
    // int32_t battery_voltage = 0;
    // if (!GetBatteryVoltageHelper(device, &battery_voltage)) {
        *message = "Unable to read battery voltage";
        return false;
    // }

    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
    //     *message = "Fastboot HAL not found";
    //     return false;
    // }

    // auto voltage_threshold = 0;
    // auto status = fastboot_hal->getBatteryVoltageFlashingThreshold(&voltage_threshold);
    // if (!status.isOk()) {
    //     *message = "Unable to get battery voltage flashing threshold";
    //     LOG(ERROR) << message->c_str() << status.getDescription();
    //     return false;
    // }
    // *message = battery_voltage >= voltage_threshold ? "yes" : "no";

    // return true;
}

bool GetOffModeChargeState(FastbootDevice* device, const std::vector<std::string>& /* args */,
                           std::string* message) {
    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
        *message = "Fastboot HAL not found";
        return false;
    // }
    // bool off_mode_charging_state = false;
    // auto status = fastboot_hal->getOffModeChargeState(&off_mode_charging_state);
    // if (!status.isOk()) {
    //     *message = "Unable to get off mode charge state";
    //     LOG(ERROR) << message->c_str() << status.getDescription();
    //     return false;
    // }
    // *message = off_mode_charging_state ? "1" : "0";
    // return true;
}

bool GetBatteryVoltage(FastbootDevice* device, const std::vector<std::string>& /* args */,
                       std::string* message) {
    int32_t battery_voltage = 0;
    // if (GetBatteryVoltageHelper(device, &battery_voltage)) {
    //     *message = std::to_string(battery_voltage);
    //     return true;
    // }
    *message = "Unable to get battery voltage";
    return false;
}

bool GetCurrentSlot(FastbootDevice* device, const std::vector<std::string>& /* args */,
                    std::string* message) {
    std::string suffix = device->GetCurrentSlot();
    *message = suffix.size() == 2 ? suffix.substr(1) : suffix;
    return true;
}

bool GetSlotCount(FastbootDevice* device, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // auto boot_control_hal = device->boot_control_hal();
    // if (!boot_control_hal) {
    //     *message = "0";
    // } else {
    //     *message = std::to_string(boot_control_hal->GetNumSlots());
    // }
    return true;
}

bool GetSlotSuccessful(FastbootDevice* device, const std::vector<std::string>& args,
                       std::string* message) {
    if (args.empty()) {
        *message = "Missing argument";
        return false;
    }
    int32_t slot = -1;
    if (!GetSlotNumber(args[0], &slot)) {
        *message = "Invalid slot";
        return false;
    }
    // auto boot_control_hal = device->boot_control_hal();
    // if (!boot_control_hal) {
    //     *message = "Device has no slots";
    //     return false;
    // }
    // if (boot_control_hal->IsSlotMarkedSuccessful(slot).value_or(false)) {
    //     *message = "no";
    // } else {
    //     *message = "yes";
    // }
    return true;
}

bool GetSlotUnbootable(FastbootDevice* device, const std::vector<std::string>& args,
                       std::string* message) {
    if (args.empty()) {
        *message = "Missing argument";
        return false;
    }
    int32_t slot = -1;
    if (!GetSlotNumber(args[0], &slot)) {
        *message = "Invalid slot";
        return false;
    }
    // auto boot_control_hal = device->boot_control_hal();
    // if (!boot_control_hal) {
    //     *message = "Device has no slots";
    //     return false;
    // }
    // if (!boot_control_hal->IsSlotBootable(slot).value_or(false)) {
    //     *message = "yes";
    // } else {
    //     *message = "no";
    // }
    return true;
}

bool GetMaxDownloadSize(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                        std::string* message) {
    *message = android::base::StringPrintf("0x%X", kMaxDownloadSizeDefault);
    return true;
}

bool GetUnlocked(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                 std::string* message) {
    *message = GetDeviceLockStatus() ? "no" : "yes";
    return true;
}

bool GetHasSlot(FastbootDevice* device, const std::vector<std::string>& args,
                std::string* message) {
    if (args.empty()) {
        *message = "Missing argument";
        return false;
    }
    std::string slot_suffix = device->GetCurrentSlot();
    if (slot_suffix.empty()) {
        *message = "no";
        return true;
    }
    std::string partition_name = args[0] + slot_suffix;
    if (FindPhysicalPartition(partition_name) || LogicalPartitionExists(device, partition_name)) {
        *message = "yes";
    } else {
        *message = "no";
    }
    return true;
}

bool GetPartitionSize(FastbootDevice* device, const std::vector<std::string>& args,
                      std::string* message) {
    if (args.size() < 1) {
        *message = "Missing argument";
        return false;
    }
    // Zero-length partitions cannot be created through device-mapper, so we
    // special case them here.
    bool is_zero_length;
    if (LogicalPartitionExists(device, args[0], &is_zero_length) && is_zero_length) {
        *message = "0x0";
        return true;
    }
    // Otherwise, open the partition as normal.
    PartitionHandle handle;
    if (!OpenPartition(device, args[0], &handle)) {
        *message = "Could not open partition";
        return false;
    }
    if (!handle.Open(O_WRONLY)){
        *message = "Could not open partition";
        return false;
    }
    uint64_t size = android::get_block_device_size(handle.fd());
    *message = android::base::StringPrintf("0x%" PRIX64, size);
    return true;
}

bool GetPartitionType(FastbootDevice* device, const std::vector<std::string>& args,
                      std::string* message) {
    if (args.size() < 1) {
        *message = "Missing argument";
        return false;
    }

    std::string partition_name = args[0];
    // if (!FindPhysicalPartition(partition_name) && !LogicalPartitionExists(device, partition_name)) {
    if (!FindPhysicalPartition(partition_name)) {
        *message = "Invalid partition";
        return false;
    }

    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
    //     *message = "raw";
    //     return true;
    // }

    // FileSystemType type;
    // auto status = fastboot_hal->getPartitionType(args[0], &type);

    // if (!status.isOk()) {
        *message = "Unable to retrieve partition type";
        // LOG(ERROR) << message->c_str() << status.getDescription();
    // } else {
    //     switch (type) {
    //         case FileSystemType::RAW:
    //             *message = "raw";
    //             return true;
    //         case FileSystemType::EXT4:
    //             *message = "ext4";
    //             return true;
    //         case FileSystemType::F2FS:
    //             *message = "f2fs";
    //             return true;
    //         default:
    //             *message = "Unknown file system type";
    //     }
    // }

    return false;
}

bool GetPartitionIsLogical(FastbootDevice* device, const std::vector<std::string>& args,
                           std::string* message) {
    if (args.size() < 1) {
        *message = "Missing argument";
        return false;
    }
    // Note: if a partition name is in both the GPT and the super partition, we
    // return "true", to be consistent with prefering to flash logical partitions
    // over physical ones.
    std::string partition_name = args[0];
    if (LogicalPartitionExists(device, partition_name)) {
        *message = "yes";
        return true;
    }
    if (FindPhysicalPartition(partition_name)) {
        *message = "no";
        return true;
    }
    *message = "Partition not found";
    return false;
}

bool GetIsUserspace(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                    std::string* message) {
    *message = "yes";
    return true;
}

bool GetIsForceDebuggable(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    *message = android::base::GetBoolProperty("ro.force.debuggable", false) ? "yes" : "no";
    return true;
}

std::vector<std::vector<std::string>> GetAllPartitionArgsWithSlot(FastbootDevice* device) {
    std::vector<std::vector<std::string>> args;
    auto partitions = ListPartitions(device);
    for (const auto& partition : partitions) {
        args.emplace_back(std::initializer_list<std::string>{partition});
    }
    return args;
}

std::vector<std::vector<std::string>> GetAllPartitionArgsNoSlot(FastbootDevice* device) {
    auto partitions = ListPartitions(device);

    std::string slot_suffix = device->GetCurrentSlot();
    if (!slot_suffix.empty()) {
        auto names = std::move(partitions);
        for (const auto& name : names) {
            std::string slotless_name = name;
            if (android::base::EndsWith(name, "_a") || android::base::EndsWith(name, "_b")) {
                slotless_name = name.substr(0, name.rfind("_"));
            }
            if (std::find(partitions.begin(), partitions.end(), slotless_name) ==
                partitions.end()) {
                partitions.emplace_back(slotless_name);
            }
        }
    }

    std::vector<std::vector<std::string>> args;
    for (const auto& partition : partitions) {
        args.emplace_back(std::initializer_list<std::string>{partition});
    }
    return args;
}

bool GetHardwareRevision(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                         std::string* message) {
    // *message = android::base::GetProperty("ro.revision", "");
    // return true;
    return false;
}

bool GetSuperPartitionName(FastbootDevice* device, const std::vector<std::string>& /* args */,
                           std::string* message) {
    // uint32_t slot_number = SlotNumberForSlotSuffix(device->GetCurrentSlot());
    // *message = fs_mgr_get_super_partition_name(slot_number);
    // return true;
    return false;
}

bool GetSnapshotUpdateStatus(FastbootDevice* device, const std::vector<std::string>& /* args */,
                             std::string* message) {
    // // Note that we use the HAL rather than mounting /metadata, since we want
    // // our results to match the bootloader.
    // auto hal = device->boot1_1();
    // if (!hal) {
        *message = "not supported";
        return false;
    // }

    // MergeStatus status = hal->getSnapshotMergeStatus();
    // switch (status) {
    //     case MergeStatus::SNAPSHOTTED:
    //         *message = "snapshotted";
    //         break;
    //     case MergeStatus::MERGING:
    //         *message = "merging";
    //         break;
    //     default:
    //         *message = "none";
    //         break;
    // }
    // return true;
}

bool GetCpuAbi(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    #ifdef __aarch64__
        *message = "arm64-v8a";
    #elif defined(__arm__)
        *message = "armeabi-v7a";
    #elif defined(__x86_64__)
        *message = "x86_64";
    #elif defined(__i386__)
        *message = "x86";
    #else
        *message = "unknown";
    #endif
    return true;
}

bool GetSystemFingerprint(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    *message = android::base::GetProperty("ro.system.build.fingerprint", "");
    if (message->empty()) {
        *message = android::base::GetProperty("ro.build.fingerprint", "");
    }
    return true;
}

bool GetVendorFingerprint(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    *message = android::base::GetProperty("ro.vendor.build.fingerprint", "");
    return true;
}

bool GetDynamicPartition(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                         std::string* message) {
    *message = android::base::GetProperty("ro.boot.dynamic_partitions", "");
    return true;
}

bool GetFirstApiLevel(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                      std::string* message) {
    *message = android::base::GetProperty("ro.product.first_api_level", "");
    return true;
}

bool GetSecurityPatchLevel(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                           std::string* message) {
    *message = android::base::GetProperty("ro.build.version.security_patch", "");
    return true;
}

bool GetTrebleEnabled(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                      std::string* message) {
    *message = android::base::GetProperty("ro.treble.enabled", "");
    return true;
}

bool GetMaxFetchSize(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                     std::string* message) {
    if (!kEnableFetch) {
        *message = "fetch not supported on user builds";
        return false;
    }
    *message = android::base::StringPrintf("0x%X", kMaxFetchSizeDefault);
    return true;
}

bool GetDmesg(FastbootDevice* device) {
    // if (GetDeviceLockStatus()) {
    //    return device->WriteFail("Cannot use when device flashing is locked");
    //}

    // Try klogctl() first
    if (ReadKernelLogNative(device)) {
        return true;
    }

    // Fallback to dmesg command
    LOG(WARNING) << "klogctl() failed, falling back to dmesg command";

    posix_spawn_file_actions_t action;
    posix_spawn_file_actions_init(&action);
    posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/dmesg.log", O_WRONLY | O_CREAT, 0644);
    posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

    char *arg[] = {"/usr/bin/dmesg", NULL};

    int subprocess_rc = -1;
    int ret = rpi::process_spawn_blocking(&subprocess_rc, "/usr/bin/dmesg", arg, NULL, &action);

    posix_spawn_file_actions_destroy(&action);
    if (ret)
    {
        device->WriteFail(strerror(errno));
        return false;
    }
    else if (subprocess_rc)
    {
        device->WriteFail("Dmesg failed");
        return false;
    }
    else
    {
        std::string dmesg_dump;
        if (android::base::ReadFileToString("/tmp/dmesg.log", &dmesg_dump))
        {
            std::istringstream stream(dmesg_dump);
            std::string line;
            while (std::getline(stream, line))
            {
                if (!device->WriteInfo(line))
                {
                    LOG(ERROR) << "Failed to write info to device";
                    return false;
                }
            }
            return true;
        }
        else
        {
            device->WriteFail("Failed to read /tmp/dmesg.log");
            return false;
        }
    }
}

bool GetPubkey(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                     std::string* message) {
    rpi::RpiFwCrypto crypto;

    auto pubkey_result = crypto.GetPublicKey();
    if (!pubkey_result) {
        *message = "Could not read public key";
        return false;
    }

    *message = *pubkey_result;
    return true;
}

bool GetPrivkey(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
    std::string* message) {
    rpi::RpiFwCrypto crypto;

    // Check if key is provisioned using cached status
    auto status = crypto.GetCachedProvisioningStatus();
    if (!status) {
        *message = "error";
        return true;
    }

    if (*status) {
        // Key is already provisioned, refuse to return it for security
        *message = "refused";
    } else {
        // Key is not provisioned, try to get it (though this might fail)
        auto privkey_result = crypto.GetPrivateKey();
        if (!privkey_result) {
            *message = "error";
        } else {
            *message = *privkey_result;
        }
    }

    return true;
}

bool GetIpv4Address(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    // Try native getifaddrs() first
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        if (GetInterfaceAddressNative(interface, IpVersion::V4, message)) {
            LOG(INFO) << "Got IPv4 address for " << interface << ": " << *message;
            return true;
        }
    }
    
    // Fallback to commands
    LOG(WARNING) << "No IPv4 address found via native interface lookup, trying fallback commands";
    std::string output;
    const char* output_file = "/tmp/ipv4-address.log";
    
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        // Try ifconfig
        if (TryExecuteCommand({"/sbin/ifconfig", "/usr/sbin/ifconfig", "/bin/ifconfig"},
                             {interface}, output_file, &output)) {
            if (ParseIpv4Address(output, message)) {
                return true;
            }
        }
        
        // Try ip command
        if (TryExecuteCommand({"/sbin/ip", "/usr/sbin/ip", "/bin/ip"},
                             {"-4", "addr", "show", "dev", interface}, output_file, &output)) {
            if (ParseIpv4Address(output, message)) {
                return true;
            }
        }
    }
    
    *message = "No IPv4 address found";
    return false;
}

bool GetIpv4Gateway(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // Try native /proc/net/route parsing first
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        if (GetIpv4GatewayNative(interface, message)) {
            LOG(INFO) << "Got IPv4 gateway for " << interface << ": " << *message;
            return true;
        }
    }
    
    // Fallback to commands
    LOG(WARNING) << "/proc parsing failed, falling back to route/ip commands";
    std::string output;
    const char* output_file = "/tmp/ipv4-gateway.log";
    
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        // Try route -n command
        if (TryExecuteCommand({"/sbin/route"}, {"-n"}, output_file, &output)) {
            if (ParseGatewayFromRouteOutput(output, interface, IpVersion::V4, message)) {
                            return true;
            }
        }
        
        // Try ip route command
        if (TryExecuteCommand({"/sbin/ip"}, {"-4", "route", "show", "dev", interface, "default"}, 
                             output_file, &output)) {
            if (ParseGatewayFromIpRouteOutput(output, message)) {
                return true;
            }
        }
    }
    
    *message = "No IPv4 gateway found";
    return false;
}

bool GetIpv4Netmask(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                   std::string* message) {
    // Try native getifaddrs() first
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        if (GetInterfaceNetmaskNative(interface, IpVersion::V4, message)) {
            LOG(INFO) << "Got IPv4 netmask for " << interface << ": " << *message;
            return true;
        }
    }

    // Fallback to commands
    LOG(WARNING) << "No IPv4 netmask found via native interface lookup, trying fallback commands";
    std::string output;
    const char* output_file = "/tmp/ipv4-netmask.log";
    
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        // Try ifconfig first
        if (TryExecuteCommand({"/sbin/ifconfig"}, {interface}, output_file, &output)) {
            if (ParseNetmaskFromIfconfig(output, IpVersion::V4, message)) {
                    return true;
            }
        }
        
        // Try ip command - returns CIDR prefix, need to convert
        if (TryExecuteCommand({"/sbin/ip"}, {"-4", "addr", "show", "dev", interface}, 
                             output_file, &output)) {
            std::string prefix;
            if (ParsePrefixFromIpOutput(output, IpVersion::V4, &prefix)) {
                try {
                    int prefix_len = std::stoi(prefix);
                    *message = CidrToNetmask(prefix_len);
                    if (!message->empty()) {
                                return true;
                            }
                        } catch (const std::exception& e) {
                    // Continue to next interface
                }
            }
        }
    }
    
    *message = "No IPv4 netmask found";
    return false;
}

bool GetIpv4Dns(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    return FindDnsServers(IpVersion::V4, message);
}

bool GetIpv6Address(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // Try native getifaddrs() first
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        if (GetInterfaceAddressNative(interface, IpVersion::V6, message)) {
            LOG(INFO) << "Got IPv6 address for " << interface << ": " << *message;
            return true;
        }
    }
    
    // Fallback to commands
    LOG(WARNING) << "No IPv6 address found via native interface lookup, trying fallback commands";
    std::string output;
    const char* output_file = "/tmp/ipv6-address.log";
    
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        // Try ifconfig
        if (TryExecuteCommand({"/sbin/ifconfig", "/usr/sbin/ifconfig", "/bin/ifconfig"},
                             {interface}, output_file, &output)) {
            if (ParseIpv6Address(output, message)) {
                return true;
            }
        }
        
        // Try ip command
        if (TryExecuteCommand({"/sbin/ip", "/usr/sbin/ip", "/bin/ip"},
                             {"-6", "addr", "show", "dev", interface}, output_file, &output)) {
            if (ParseIpv6Address(output, message)) {
                    return true;
                }
        }
    }
    
    *message = "No IPv6 address found";
    return false;
}

bool GetIpv6Gateway(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // Try native /proc/net/ipv6_route parsing first
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        if (GetIpv6GatewayNative(interface, message)) {
            LOG(INFO) << "Got IPv6 gateway for " << interface << ": " << *message;
            return true;
        }
    }
    
    // Fallback to commands
    LOG(WARNING) << "/proc parsing failed, falling back to ip commands";
    std::string output;
    const char* output_file = "/tmp/ipv6-gateway.log";
    
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        // Try route -6 -n command
        if (TryExecuteCommand({"/sbin/route"}, {"-n", "-6"}, output_file, &output)) {
            if (ParseGatewayFromRouteOutput(output, interface, IpVersion::V6, message)) {
                            return true;
            }
        }
        
        // Try ip route command
        if (TryExecuteCommand({"/sbin/ip"}, {"-6", "route", "show", "dev", interface, "default"}, 
                             output_file, &output)) {
            if (ParseGatewayFromIpRouteOutput(output, message)) {
                return true;
            }
        }
    }
    
    *message = "No IPv6 gateway found";
    return false;
}

bool GetIpv6Netmask(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                   std::string* message) {
    // Try native getifaddrs() first (returns prefix length, e.g. "64")
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        if (GetInterfaceNetmaskNative(interface, IpVersion::V6, message)) {
            LOG(INFO) << "Got IPv6 prefix for " << interface << ": " << *message;
            return true;
        }
    }

    // Fallback to commands
    LOG(WARNING) << "No IPv6 prefix found via native interface lookup, trying fallback commands";
    std::string output;
    const char* output_file = "/tmp/ipv6-netmask.log";
    
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        const char* interface = kNetworkInterfaces[i];
        
        // Try ifconfig first
        if (TryExecuteCommand({"/sbin/ifconfig"}, {interface}, output_file, &output)) {
            if (ParseNetmaskFromIfconfig(output, IpVersion::V6, message)) {
                            return true;
            }
        }
        
        // Try ip command
        if (TryExecuteCommand({"/sbin/ip"}, {"-6", "addr", "show", "dev", interface}, 
                             output_file, &output)) {
            if (ParsePrefixFromIpOutput(output, IpVersion::V6, message)) {
                            return true;
            }
        }
    }
    
    *message = "No IPv6 prefix length found";
    return false;
}

bool GetIpv6Dhcp(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    static const DhcpConfig dhcpv6_config = {
        ":546",  // client port
        ":547",  // server port
        {"dhclient", "dhcp6c"},  // process names (dhclient with -6 flag checked in DetectDhcp)
        {"/var/lib/dhcp/dhclient6.%s.leases", "/var/lib/dhcp6c/dhcp6c_%s.lease"}
    };
    
    // Try both interfaces
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        if (DetectDhcp(kNetworkInterfaces[i], dhcpv6_config, message)) {
            if (*message == "yes") {
                return true;
            }
        }
    }
    
    *message = "no";
    return true;
}

bool GetIpv6Dns(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    return FindDnsServers(IpVersion::V6, message);
}

bool GetIpv4Dhcp(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    static const DhcpConfig dhcp_config = {
        ":68",  // client port
        ":67",  // server port
        {"dhclient", "dhcpcd"},  // process names
        {"/var/lib/dhcp/dhclient.%s.leases", "/var/lib/dhcpcd/dhcpcd-%s.lease"}
    };
    
    // Try both interfaces
    for (size_t i = 0; i < kNumNetworkInterfaces; ++i) {
        if (DetectDhcp(kNetworkInterfaces[i], dhcp_config, message)) {
            if (*message == "yes") {
                return true;
            }
        }
    }
    
    *message = "no";
    return true;
}


