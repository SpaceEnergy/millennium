#include "co_stub.h"
#include <vector>
#include <Python.h>
#include <fmt/core.h>
#include <core/py_controller/co_spawn.h>
#include <sys/log.h>
#include <core/loader.h>
#include <core/hooks/web_load.h>
#include <core/ffi/ffi.h>
#include <tuple>

const std::string GetBootstrapModule(const std::string scriptModules, const uint16_t port)
{
    return R"(
function createWebSocket(url) {
    return new Promise((resolve, reject) => {
        try {
            let socket = new WebSocket(url);
            socket.addEventListener('open', () => {
                console.log('%c Millennium ', 'background: black; color: white', "Successfully connected to IPC server.");
                resolve(socket);
            });
            socket.addEventListener('close', () => {
                setTimeout(() => {
                    createWebSocket(url).then(resolve).catch(reject);
                }, 100);
            });
        } 
        catch (error) {
            console.warn('Failed to connect to IPC server:', error);
        } 
    });
}

function waitForSocket(socket) {
    return new Promise((resolve, reject) => {
        if (socket.readyState === WebSocket.OPEN) {
            resolve();
        } else {
            socket.addEventListener('open', resolve);
            socket.addEventListener('error', reject);
        }
    });
}

const InjectLegacyReactGlobals = () => {

    let initReq;
    let bufferWebpackCache = {};
    window.webpackChunksteamui?.push([[Math.random()], {}, (r) => { initReq = r; }]);

    for (let i of Object.keys(initReq.m)) {
        try {
            bufferWebpackCache[i] = initReq(i);
        } 
        catch (e) { }
    }

    const findModule = (filter) => {
        const allModules = Object.values(bufferWebpackCache).filter((x) => x)

        for (const m of allModules) {
            if (m.default && filter(m.default)) return m.default;
            if (filter(m)) return m;
        }
    };

    window.SP_REACTDOM = findModule((module) => module.createPortal && module.createRoot && module.flushSync)
    window.SP_REACT    = findModule((module) => module.Component && module.PureComponent && module.useLayoutEffect);
}

function waitForSPReactDOM() {
    return new Promise((resolve) => {
        const interval = setInterval(() => {
            if (window?.webpackChunksteamui?.length >= 5) {
                InjectLegacyReactGlobals();
                clearInterval(interval);
                resolve();
            }
        }, 100);
    });
}

createWebSocket('ws://localhost:)" + std::to_string(port) + R"(').then((socket) => {
    window.MILLENNIUM_IPC_SOCKET = socket;
    window.CURRENT_IPC_CALL_COUNT = 0;

    Promise.all([ waitForSocket(socket), waitForSPReactDOM() ]).then(() => {
        console.log('%c Millennium ', 'background: black; color: white', "Ready to inject shims...");
        )" + scriptModules + R"(
    })
    .catch((error) => console.error('error @ createWebSocket ->', error));
})
.catch((error) => console.error('Initial WebSocket connection failed:', error));
)";
}

/// @brief sets up the python interpreter to use virtual environment site packages, as well as custom python path.
/// @param system path 
/// @return void 
const void AppendSysPathModules(std::vector<std::filesystem::path> sitePackages) 
{
    PyObject *sysModule = PyImport_ImportModule("sys");
    if (!sysModule) 
    {
        LOG_ERROR("couldn't import system module");
        return;
    }

    PyObject *systemPath = PyObject_GetAttrString(sysModule, "path");

    if (systemPath) 
    {
#ifdef _WIN32
        // Wipe the system path clean when on windows
        // - Prevents clashing installed python versions
        PyList_SetSlice(systemPath, 0, PyList_Size(systemPath), NULL);
#endif

        for (const auto& systemPathItem : sitePackages) 
        {
            PyList_Append(systemPath, PyUnicode_FromString(systemPathItem.generic_string().c_str()));
        }
        Py_DECREF(systemPath);
    }
    Py_DECREF(sysModule);
}

void AddSitePackagesDirectory(std::filesystem::path customPath)
{
    PyObject *siteModule = PyImport_ImportModule("site");

    if (!siteModule) 
    {
        PyErr_Print();
        LOG_ERROR("couldn't import site module");
        return;
    }

    PyObject *addSiteDirFunc = PyObject_GetAttrString(siteModule, "addsitedir");
    if (addSiteDirFunc && PyCallable_Check(addSiteDirFunc)) 
    {
        PyObject *args = PyTuple_Pack(1, PyUnicode_FromString(customPath.generic_string().c_str()));
        PyObject *result = PyObject_CallObject(addSiteDirFunc, args);
        Py_XDECREF(result);
        Py_XDECREF(args);
        Py_XDECREF(addSiteDirFunc);
    } 
    else 
    {
        PyErr_Print();
        LOG_ERROR("Failed to get addsitedir function");
    }
    Py_XDECREF(siteModule);
}


/// @brief initializes the current plugin. creates a plugin instance and calls _load()
/// @param global_dict 
void StartPluginBackend(PyObject* global_dict, std::string pluginName) 
{
    const auto PrintError = [&pluginName]() 
    {
        const auto [errorMessage, traceback] = Python::GetExceptionInformaton();
        PyErr_Clear();

        if (errorMessage == "name 'plugin' is not defined")
        {
            Logger.PrintMessage(" FFI-ERROR ", fmt::format("Millennium failed to call _load on {}", pluginName), COL_RED);
            return;
        }

        Logger.PrintMessage(" FFI-ERROR ", fmt::format("Millennium failed to call _load on {}: {}\n{}{}", pluginName, COL_RED, traceback, COL_RESET), COL_RED);
    };

    PyObject *pluginComponent = PyDict_GetItemString(global_dict, "Plugin");

    if (!pluginComponent || !PyCallable_Check(pluginComponent)) 
    {
        PrintError();
        return;
    }

    PyObject *pluginComponentInstance = PyObject_CallObject(pluginComponent, NULL);

    if (!pluginComponentInstance) 
    {
        PrintError();
        return;
    }

    PyDict_SetItemString(global_dict, "plugin", pluginComponentInstance);
    PyObject *loadMethodAttribute = PyObject_GetAttrString(pluginComponentInstance, "_load");

    if (!loadMethodAttribute || !PyCallable_Check(loadMethodAttribute)) 
    {
        PrintError();
        return;
    }

    PyObject_CallObject(loadMethodAttribute, NULL);
    Py_DECREF(loadMethodAttribute);
    Py_DECREF(pluginComponentInstance);
}

const void SetPluginSecretName(PyObject* globalDictionary, const std::string& pluginName) 
{
    PyDict_SetItemString(globalDictionary, "MILLENNIUM_PLUGIN_SECRET_NAME", PyUnicode_FromString(pluginName.c_str()));
}

const void CoInitializer::BackendStartCallback(SettingsStore::PluginTypeSchema plugin) 
{
    PyObject* globalDictionary = PyModule_GetDict(PyImport_AddModule("__main__"));

    const auto backendMainModule = plugin.backendAbsoluteDirectory.generic_string();
    const auto pluginVirtualEnv  = plugin.pluginBaseDirectory / ".millennium";

    // associate the plugin name with the running plugin. used for IPC/FFI
    SetPluginSecretName(globalDictionary, plugin.pluginName);

    std::vector<std::filesystem::path> sysPath;
    sysPath.push_back(plugin.pluginBaseDirectory / plugin.backendAbsoluteDirectory.parent_path());

    #ifdef _WIN32
    {
        /* Add local python binaries to virtual PATH to prevent changing actual PATH */
        AddDllDirectory(pythonModulesBaseDir.wstring().c_str());

        sysPath.push_back(pythonPath);
        sysPath.push_back(pythonLibs);
    }
    #endif

    AppendSysPathModules(sysPath);

    #ifdef _WIN32
    AddSitePackagesDirectory(SystemIO::GetInstallPath() / "ext" / "data" / "cache" / "Lib" / "site-packages");
    #else
    AddSitePackagesDirectory(SystemIO::GetInstallPath() / "ext" / "data" / "cache" / "lib" / "python3.11" / "site-packages");
    #endif

    AddSitePackagesDirectory(pluginVirtualEnv);
    CoInitializer::BackendCallbacks& backendHandler = CoInitializer::BackendCallbacks::getInstance();

    PyObject *mainModuleObj = Py_BuildValue("s", backendMainModule.c_str());
    FILE *mainModuleFilePtr = _Py_fopen_obj(mainModuleObj, "r+");

    if (mainModuleFilePtr == NULL) 
    {
        Logger.Warn("failed to fopen file @ {}", backendMainModule);
        backendHandler.BackendLoaded({ plugin.pluginName, CoInitializer::BackendCallbacks::BACKEND_LOAD_FAILED });
        return;
    }

    PyObject* mainModule = PyImport_AddModule("__main__");
    PyObject* mainModuleDict = PyModule_GetDict(mainModule);

    if (!mainModule || !mainModuleDict) {
        Logger.Warn("Millennium failed to initialize the main module.");
        backendHandler.BackendLoaded({ plugin.pluginName, CoInitializer::BackendCallbacks::BACKEND_LOAD_FAILED });
        fclose(mainModuleFilePtr);
        return;
    }

    PyObject* result = PyRun_File(mainModuleFilePtr, backendMainModule.c_str(), Py_file_input, mainModuleDict, mainModuleDict);
    fclose(mainModuleFilePtr);

    if (!result) 
    {
        const auto [errorMessage, traceback] = Python::GetExceptionInformaton();

        Logger.PrintMessage(" PY-MAN ", fmt::format("Millennium failed to start {}: {}\n{}{}", plugin.pluginName, COL_RED, traceback, COL_RESET), COL_RED);
        Logger.Warn("Millennium failed to start '{}'. This is likely due to failing module side effects, unrelated to Millennium.", plugin.pluginName);
        backendHandler.BackendLoaded({ plugin.pluginName, CoInitializer::BackendCallbacks::BACKEND_LOAD_FAILED });
        return;
    }

    Py_DECREF(result);
    StartPluginBackend(globalDictionary, plugin.pluginName);  
}

const std::string ConstructScriptElement(std::string filename) 
{
    return fmt::format("\nif (!document.querySelectorAll(`script[src='{}'][type='module']`).length) {{ document.head.appendChild(Object.assign(document.createElement('script'), {{ src: '{}', type: 'module', id: 'millennium-injected' }})); }}", filename, filename);
}

const std::string ConstructOnLoadModule(uint16_t ftpPort, uint16_t ipcPort) 
{
    std::unique_ptr<SettingsStore> settingsStore = std::make_unique<SettingsStore>();
    std::vector<SettingsStore::PluginTypeSchema> plugins = settingsStore->ParseAllPlugins();

    std::string scriptImportTable;
    
    for (auto& plugin : plugins)  
    {
        if (!settingsStore->IsEnabledPlugin(plugin.pluginName)) 
        {    
            continue;
        }

        const auto frontEndAbs = plugin.frontendAbsoluteDirectory.generic_string();
        const std::string pathShim = plugin.isInternal ? "_internal_/" : std::string();

        scriptImportTable.append(ConstructScriptElement(fmt::format("http://localhost:{}/{}{}", ftpPort, pathShim, frontEndAbs)));
    }

    return GetBootstrapModule(scriptImportTable, ipcPort);
}

static std::string addedScriptOnNewDocumentId;

#include <mutex>
#include <condition_variable>
#include <thread>

void OnBackendLoad(uint16_t ftpPort, uint16_t ipcPort)
{
    Logger.Log("Notifying frontend of backend load...");

    static uint16_t m_ftpPort = ftpPort;
    static uint16_t m_ipcPort = ipcPort;

    enum PageMessage
    {
        DEBUGGER_RESUME = 1,
        PAGE_ENABLE = 2,
        PAGE_SCRIPT = 3,
        PAGE_RELOAD = 4
    };

    std::mutex mtx;
    std::condition_variable cvDebugger;
    std::condition_variable cvScript;

    bool hasUnpausedDebugger = false;
    bool hasScriptIdentifier = false;

    auto socketEmitterThread = std::thread([&]
    {
        JavaScript::SharedJSMessageEmitter::InstanceRef().OnMessage("msg", 
            [&] (const nlohmann::json& eventMessage, int listenerId)
        {
            std::unique_lock<std::mutex> lock(mtx);
            
            try
            {
                const int messageId = eventMessage.value("id", -1);

                if (messageId == DEBUGGER_RESUME)
                {
                    if (eventMessage.contains("error"))
                    {
                        hasUnpausedDebugger = false;
                        Logger.Warn("Failed to resume debugger, Steam is likely not yet loaded...");
                    }
                    else if (eventMessage.contains("result"))
                    {
                        hasUnpausedDebugger = true;
                        Logger.Log("Successfully resumed debugger, injecting shims...");
                        Sockets::PostShared({ {"id", PAGE_ENABLE }, {"method", "Page.enable"} });
                        Sockets::PostShared({ {"id", PAGE_SCRIPT }, {"method", "Page.addScriptToEvaluateOnNewDocument"}, {"params", {{ "source", ConstructOnLoadModule(m_ftpPort, m_ipcPort) }}} });
                    }
                    cvDebugger.notify_one();  // Notify that debugger resume is processed
                }
                else if (messageId == PAGE_SCRIPT)
                {
                    addedScriptOnNewDocumentId = eventMessage["result"]["identifier"];
                    hasScriptIdentifier = true;
                    Logger.Log("Successfully injected shims, updating state...");
                    Sockets::PostShared({ {"id", PAGE_RELOAD }, {"method", "Page.reload"} });
                    cvScript.notify_one();  // Notify that script injection is processed
                }

                // Check if both conditions have been met
                if (hasUnpausedDebugger && hasScriptIdentifier)
                {
                    Logger.Log("Successfully notified frontend...");
                    JavaScript::SharedJSMessageEmitter::InstanceRef().RemoveListener("msg", listenerId);
                }
            }
            catch (nlohmann::detail::exception& ex)
            {
                LOG_ERROR("JavaScript::SharedJSMessageEmitter error -> {}", ex.what());
            }
        });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Sockets::PostShared({ {"id", DEBUGGER_RESUME }, {"method", "Debugger.resume"} });

    // Wait for debugger resume
    {
        std::unique_lock<std::mutex> lock(mtx);
        cvDebugger.wait(lock, [&] { return hasUnpausedDebugger; });
    }

    // Wait for script injection completion
    {
        std::unique_lock<std::mutex> lock(mtx);
        cvScript.wait(lock, [&] { return hasScriptIdentifier; });
    }

    socketEmitterThread.join();
    Logger.Log("Frontend notifier finished!");
}

const void CoInitializer::InjectFrontendShims(uint16_t ftpPort, uint16_t ipcPort) 
{
    std::mutex mtx;
    std::condition_variable cv;
    bool hasSuccess = false, hasPaused = false;

    Logger.Log("Preparing to inject frontend shims...");

    JavaScript::SharedJSMessageEmitter::InstanceRef().OnMessage("msg", [&](const nlohmann::json& eventMessage, int listenerId) 
    {
        std::lock_guard<std::mutex> lock(mtx);
        
        if (eventMessage.value("id", -1) == 65756) 
        {
            if (eventMessage.contains("error")) 
            {
                Logger.Warn("Failed to pause debugger, Steam is likely not yet loaded...");
                Sockets::PostShared({ {"id", 65756 }, {"method", "Debugger.pause"} });
            } 
            else if (eventMessage.contains("result")) 
            {
                Logger.Log("Successfully sent debugger pause...");
                hasSuccess = true;
            }
        }
        if (eventMessage.contains("method") && eventMessage["method"] == "Debugger.paused") 
        {
            Logger.Log("Debugger has paused!");
            hasPaused = true;
        }

        if (hasSuccess && hasPaused) 
        {
            try 
            {
                JavaScript::SharedJSMessageEmitter::InstanceRef().RemoveListener("msg", listenerId);
            } 
            catch (const std::exception& ex) 
            {
                LOG_ERROR("Error removing listener: {}", ex.what());
            }
            cv.notify_all(); 
        }
    });

    Sockets::PostShared({ {"id", 3422 }, {"method", "Debugger.enable"} });
    Sockets::PostShared({ {"id", 65756 }, {"method", "Debugger.pause"} });

    try 
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return hasSuccess && hasPaused; });
    } 
    catch (const std::system_error& e)
    {
        LOG_ERROR("Condition variable wait error: {}", e.what());
        return;
    }

    Logger.Log("Ready to inject shims!");
    BackendCallbacks& backendHandler = BackendCallbacks::getInstance();
    backendHandler.RegisterForLoad(std::bind(OnBackendLoad, ftpPort, ipcPort));
}

const void CoInitializer::ReInjectFrontendShims()
{
    Sockets::PostShared({ {"id", 0 }, {"method", "Page.removeScriptToEvaluateOnNewDocument"}, {"params", {{ "identifier", addedScriptOnNewDocumentId }}} });
    InjectFrontendShims();
}
