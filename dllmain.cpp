// MIT License
//
// Copyright(c) 2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

namespace {

    // Handle and function pointers to the chained runtime library.
    wil::unique_hmodule chainedRuntimeModule;
    PFN_xrNegotiateLoaderRuntimeInterface next_xrNegotiateLoaderRuntimeInterface = nullptr;
    PFN_xrGetInstanceProcAddr next_xrGetInstanceProcAddr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties next_xrEnumerateInstanceExtensionProperties = nullptr;

    // The list of instance extensions to mask, loaded from our configuration file.
    std::vector<std::string> extensionsToMask;

    std::ofstream logStream;

    // Basic logging function.
    void Log(const char* fmt, ...) {
        const std::time_t now = std::time(nullptr);
        char buf[1024];
        size_t offset = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %z: ", std::localtime(&now));
        va_list va;
        va_start(va, fmt);
        vsnprintf_s(buf + offset, sizeof(buf) - offset, _TRUNCATE, fmt, va);
        va_end(va);

        OutputDebugStringA(buf);
        if (logStream.is_open()) {
            logStream << buf;
            logStream.flush();
        }
    }

    // Our own implementation of the instance extensions enumeration, so we can mask certain extensions.
    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateInstanceExtensionProperties
    XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName,
                                                               uint32_t propertyCapacityInput,
                                                               uint32_t* propertyCountOutput,
                                                               XrExtensionProperties* properties) {
        XrResult result;
        if (!layerName) {
            // Because we alter the number of extensions, we must always perform a first call to get the real number of
            // extensions.
            result = next_xrEnumerateInstanceExtensionProperties(nullptr, 0, propertyCountOutput, nullptr);
            if (XR_SUCCEEDED(result)) {
                // Query the real list of extensions.
                std::vector<XrExtensionProperties> propertiesArray(*propertyCountOutput,
                                                                   {XR_TYPE_EXTENSION_PROPERTIES});
                result = next_xrEnumerateInstanceExtensionProperties(
                    nullptr, (uint32_t)propertiesArray.size(), propertyCountOutput, propertiesArray.data());
                if (XR_SUCCEEDED(result)) {
                    // Mask out the desired extensions.
                    for (const std::string& extensionToMask : extensionsToMask) {
                        const auto it = std::find_if(propertiesArray.begin(),
                                                     propertiesArray.end(),
                                                     [&extensionToMask](const XrExtensionProperties& properties) {
                                                         return properties.extensionName == extensionToMask;
                                                     });
                        if (it != propertiesArray.end()) {
                            propertiesArray.erase(it);
                        }
                    }

                    // Output the edited list if needed.
                    if (properties) {
                        memcpy(
                            properties, propertiesArray.data(), propertiesArray.size() * sizeof(XrExtensionProperties));
                    }

                    // Always return the adjusted count.
                    *propertyCountOutput = (uint32_t)propertiesArray.size();
                    if (propertyCapacityInput && propertyCapacityInput < *propertyCountOutput) {
                        result = XR_ERROR_SIZE_INSUFFICIENT;
                    }
                }
            }
        } else {
            result = next_xrEnumerateInstanceExtensionProperties(
                layerName, propertyCapacityInput, propertyCountOutput, properties);
        }

        return result;
    }

    // Our proxy implementation of xrGetInstanceProcAddr() to override any function.
    XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
        const XrResult result = next_xrGetInstanceProcAddr(instance, name, function);

        if (XR_SUCCEEDED(result)) {
            const std::string_view functionName = name;
            if (functionName == "xrEnumerateInstanceExtensionProperties") {
                // Remember where the real xrEnumerateInstanceExtensionProperties() is.
                next_xrEnumerateInstanceExtensionProperties =
                    reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(*function);

                // Tell the loader to use our own implementation of xrEnumerateInstanceExtensionProperties().
                *function = reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateInstanceExtensionProperties);
                return XR_SUCCESS;
            }
        }

        return result;
    }

    void initializeWrapper() {
        // Create a log file for troubleshooting.
        std::filesystem::path logPath =
            (std::filesystem::path(getenv("LOCALAPPDATA")) / (std::string(PROJECTNAME) + ".log"));
        logStream.open(logPath, std::ios_base::ate);

        // Load the configuration.
        std::filesystem::path openXrRuntime;
        {
            // Retrieve the path of the DLL.
            std::filesystem::path dllHome;
            HMODULE module;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)&chainedRuntimeModule,
                                   &module)) {
                wchar_t path[_MAX_PATH];
                GetModuleFileNameW(module, path, sizeof(path));
                dllHome = std::filesystem::path(path).parent_path();
            }

            // Read the configuration.
            std::filesystem::path configPath = dllHome / (std::string(PROJECTNAME) + ".cfg");
            std::ifstream configFile;
            configFile.open(configPath);
            if (configFile.is_open()) {
                unsigned int lineNumber = 0;
                std::string line;
                while (std::getline(configFile, line)) {
                    lineNumber++;
                    try {
                        const auto offset = line.find('=');
                        if (offset != std::string::npos) {
                            const std::string name = line.substr(0, offset);
                            const std::string value = line.substr(offset + 1);

                            if (name == "runtime") {
                                openXrRuntime = dllHome / (value + ".dll");
                            } else if (name == "maskExtension") {
                                extensionsToMask.push_back(value);
                                Log("Masking extension: %s\n", value.c_str());
                            } else {
                                Log("L%u: Unrecognized option `%s'\n", lineNumber, name.c_str());
                            }
                        } else {
                            Log("L%u: Improperly formatted option\n", lineNumber);
                        }
                    } catch (...) {
                        Log("L%u: Parsing error\n", lineNumber);
                    }
                }
                configFile.close();
            } else {
                Log("Failed to open file `%ls'\n", configPath.c_str());
            }
        }

        // Load the library for the real OpenXR runtime.
        if (!openXrRuntime.empty()) {
            Log("Loading runtime `%ls'\n", openXrRuntime.c_str());
            *chainedRuntimeModule.put() = LoadLibraryW(openXrRuntime.c_str());
            if (chainedRuntimeModule) {
                next_xrNegotiateLoaderRuntimeInterface = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
                    GetProcAddress(chainedRuntimeModule.get(), "xrNegotiateLoaderRuntimeInterface"));
            } else {
                Log("Failed to load runtime `%ls'\n", openXrRuntime.c_str());
            }
        }
    }

} // namespace

// Entry point for the loader.
extern "C" {
XrResult __declspec(dllexport) XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                            XrNegotiateRuntimeRequest* runtimeRequest) {
    // The loader typically returns XR_ERROR_FILE_ACCESS_ERROR when failing to load any DLL.
    if (!chainedRuntimeModule) {
        return XR_ERROR_FILE_ACCESS_ERROR;
    }

    // Call the real OpenXR runtime.
    const XrResult result = next_xrNegotiateLoaderRuntimeInterface(loaderInfo, runtimeRequest);
    if (XR_SUCCEEDED(result)) {
        // Remember where the real implementation of xrGetInstanceProcAddr() is.
        next_xrGetInstanceProcAddr = runtimeRequest->getInstanceProcAddr;

        // Tell the loader to use our own implementation of xrGetInstanceProcAddr().
        runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr;
    }

    return result;
}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        initializeWrapper();
        break;

    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
