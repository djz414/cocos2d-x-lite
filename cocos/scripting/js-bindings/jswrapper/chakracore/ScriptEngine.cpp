#include "ScriptEngine.hpp"

#if SCRIPT_ENGINE_TYPE == SCRIPT_ENGINE_CHAKRACORE

#include "Object.hpp"
#include "Class.hpp"
#include "Utils.hpp"
#include "../State.hpp"
#include "../MappingUtils.hpp"

namespace se {

    Class* __jsb_CCPrivateData_class = nullptr;

    namespace {
        ScriptEngine* __instance = nullptr;

        JsValueRef __forceGC(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
        {
            ScriptEngine::getInstance()->garbageCollect();
            return JS_INVALID_REFERENCE;
        }

        JsValueRef __log(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
        {
            if (argumentCount > 1)
            {
                std::string str;
                internal::forceConvertJsValueToStdString(arguments[1], &str);
                LOGD("JS: %s\n", str.c_str());
            }
            return JS_INVALID_REFERENCE;
        }

        void myJsBeforeCollectCallback(void *callbackState)
        {
            LOGD("GC start ...\n");
        }

        JsValueRef privateDataContructor(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
        {
            return JS_INVALID_REFERENCE;
        }

        void privateDataFinalize(void *data)
        {
            internal::PrivateData* p = (internal::PrivateData*)data;
            if (p->finalizeCb != nullptr)
                p->finalizeCb(p->data);
            free(p);
        }

        // For console stuff
        bool JSB_console_format_log(State& s, const char* prefix, int msgIndex = 0)
        {
            if (msgIndex < 0)
                return false;

            const auto& args = s.args();
            int argc = (int)args.size();
            if ((argc - msgIndex) == 1)
            {
                std::string msg = args[msgIndex].toStringForce();
                LOGD("JS: %s%s\n", prefix, msg.c_str());
            }
            else if (argc > 1)
            {
                std::string msg = args[msgIndex].toStringForce();
                size_t pos;
                for (int i = (msgIndex+1); i < argc; ++i)
                {
                    pos = msg.find("%");
                    if (pos != std::string::npos && pos != (msg.length()-1) && (msg[pos+1] == 'd' || msg[pos+1] == 's' || msg[pos+1] == 'f'))
                    {
                        msg.replace(pos, 2, args[i].toStringForce());
                    }
                    else
                    {
                        msg += " " + args[i].toStringForce();
                    }
                }

                LOGD("JS: %s%s\n", prefix, msg.c_str());
            }

            return true;
        }

        bool JSB_console_log(State& s)
        {
            JSB_console_format_log(s, "");
            return true;
        }
        SE_BIND_FUNC(JSB_console_log)

        bool JSB_console_debug(State& s)
        {
            JSB_console_format_log(s, "[DEBUG]: ");
            return true;
        }
        SE_BIND_FUNC(JSB_console_debug)

        bool JSB_console_info(State& s)
        {
            JSB_console_format_log(s, "[INFO]: ");
            return true;
        }
        SE_BIND_FUNC(JSB_console_info)

        bool JSB_console_warn(State& s)
        {
            JSB_console_format_log(s, "[WARN]: ");
            return true;
        }
        SE_BIND_FUNC(JSB_console_warn)

        bool JSB_console_error(State& s)
        {
            JSB_console_format_log(s, "[ERROR]: ");
            return true;
        }
        SE_BIND_FUNC(JSB_console_error)

        bool JSB_console_assert(State& s)
        {
            const auto& args = s.args();
            if (!args.empty())
            {
                if (args[0].isBoolean() && !args[0].toBoolean())
                {
                    JSB_console_format_log(s, "[ASSERT]: ", 1);
                }
            }
            return true;
        }
        SE_BIND_FUNC(JSB_console_assert)

    } // namespace {

    ScriptEngine *ScriptEngine::getInstance()
    {
        if (__instance == nullptr)
        {
            __instance = new ScriptEngine();
        }

        return __instance;
    }

    void ScriptEngine::destroyInstance()
    {
        delete __instance;
        __instance = nullptr;
    }

    ScriptEngine::ScriptEngine()
            : _rt(JS_INVALID_RUNTIME_HANDLE)
            , _cx(JS_INVALID_REFERENCE)
            , _globalObj(nullptr)
            , _exceptionCallback(nullptr)
            , _currentSourceContext(0)
            , _vmId(0)
            , _isValid(false)
            , _isInCleanup(false)
            , _isErrorHandleWorking(false)
            , _isGarbageCollecting(false)
    {
    }

    bool ScriptEngine::init()
    {
        cleanup();
        LOGD("Initializing ChakraCore, version: %d.%d.%d\n", CHAKRA_CORE_MAJOR_VERSION, CHAKRA_CORE_MINOR_VERSION, CHAKRA_CORE_PATCH_VERSION);

        ++_vmId;
        for (const auto& hook : _beforeInitHookArray)
        {
            hook();
        }
        _beforeInitHookArray.clear();

        _CHECK(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &_rt));
        _CHECK(JsCreateContext(_rt, &_cx));
        _CHECK(JsSetCurrentContext(_cx));

        NativePtrToObjectMap::init();
        NonRefNativePtrCreatedByCtorMap::init();

        // Set up ES6 Promise
//        if (JsSetPromiseContinuationCallback(PromiseContinuationCallback, &taskQueue) != JsNoError)

        JsValueRef globalObj = JS_INVALID_REFERENCE;
        _CHECK(JsGetGlobalObject(&globalObj));

        _CHECK(JsSetRuntimeBeforeCollectCallback(_rt, nullptr, myJsBeforeCollectCallback));

        _globalObj = Object::_createJSObject(nullptr, globalObj);
        _globalObj->root();

        // ChakraCore isn't shipped with a console variable. Make a fake one.
        Value consoleVal;
        bool hasConsole = _globalObj->getProperty("console", &consoleVal) && consoleVal.isObject();
        assert(!hasConsole);

        HandleObject consoleObj(Object::createPlainObject());
        consoleObj->defineFunction("log", _SE(JSB_console_log));
        consoleObj->defineFunction("debug", _SE(JSB_console_debug));
        consoleObj->defineFunction("info", _SE(JSB_console_info));
        consoleObj->defineFunction("warn", _SE(JSB_console_warn));
        consoleObj->defineFunction("error", _SE(JSB_console_error));
        consoleObj->defineFunction("assert", _SE(JSB_console_assert));

        _globalObj->setProperty("console", Value(consoleObj));

        _globalObj->setProperty("scriptEngineType", Value("ChakraCore"));

        _globalObj->defineFunction("log", __log);
        _globalObj->defineFunction("forceGC", __forceGC);

        __jsb_CCPrivateData_class = Class::create("__CCPrivateData", _globalObj, nullptr, privateDataContructor);
        __jsb_CCPrivateData_class->defineFinalizeFunction(privateDataFinalize);
        __jsb_CCPrivateData_class->install();

        _isValid = true;

        for (const auto& hook : _afterInitHookArray)
        {
            hook();
        }
        _afterInitHookArray.clear();

        return true;
    }

    ScriptEngine::~ScriptEngine()
    {
        cleanup();
    }

    void ScriptEngine::cleanup()
    {
        if (!_isValid)
            return;

        _isInCleanup = true;
        for (const auto& hook : _beforeCleanupHookArray)
        {
            hook();
        }
        _beforeCleanupHookArray.clear();

        SAFE_DEC_REF(_globalObj);
        Object::cleanup();
        Class::cleanup();
        garbageCollect();

        _CHECK(JsSetCurrentContext(JS_INVALID_REFERENCE));
        _CHECK(JsDisposeRuntime(_rt));

        _cx = nullptr;
        _globalObj = nullptr;
        _isValid = false;

        _registerCallbackArray.clear();

        for (const auto& hook : _afterCleanupHookArray)
        {
            hook();
        }
        _afterCleanupHookArray.clear();
        _isInCleanup = false;

        NativePtrToObjectMap::destroy();
        NonRefNativePtrCreatedByCtorMap::destroy();
    }

    ScriptEngine::ExceptionInfo ScriptEngine::formatException(JsValueRef exception)
    {
        ExceptionInfo ret;
        if (exception == JS_INVALID_REFERENCE)
            return ret;

        std::vector<std::string> allKeys;
        Object* exceptionObj = Object::_createJSObject(nullptr, exception);
        exceptionObj->getAllKeys(&allKeys);

        for (const auto& key : allKeys)
        {
            Value tmp;
            if (exceptionObj->getProperty(key.c_str(), &tmp))
            {
//                LOGD("[%s]=%s\n", key.c_str(), tmp.toStringForce().c_str());
                if (key == "message")
                {
                    ret.message = tmp.toString();
                }
                else if (key == "stack")
                {
                    ret.stack = tmp.toString();
                }
            }
        }

        exceptionObj->decRef();

        ret.location = "(see stack)";

        return ret;
    }

    Object* ScriptEngine::getGlobalObject()
    {
        return _globalObj;
    }

    void ScriptEngine::addBeforeInitHook(const std::function<void()>& hook)
    {
        _beforeInitHookArray.push_back(hook);
    }

    void ScriptEngine::addAfterInitHook(const std::function<void()>& hook)
    {
        _afterInitHookArray.push_back(hook);
    }

    void ScriptEngine::addBeforeCleanupHook(const std::function<void()>& hook)
    {
        _beforeCleanupHookArray.push_back(hook);
    }

    void ScriptEngine::addAfterCleanupHook(const std::function<void()>& hook)
    {
        _afterCleanupHookArray.push_back(hook);
    }

    void ScriptEngine::addRegisterCallback(RegisterCallback cb)
    {
        assert(std::find(_registerCallbackArray.begin(), _registerCallbackArray.end(), cb) == _registerCallbackArray.end());
        _registerCallbackArray.push_back(cb);
    }

    bool ScriptEngine::start()
    {
        if (!init())
            return false;

        bool ok = false;
        _startTime = std::chrono::steady_clock::now();

        for (auto cb : _registerCallbackArray)
        {
            ok = cb(_globalObj);
            assert(ok);
            if (!ok)
                break;
        }

        // After ScriptEngine is started, _registerCallbackArray isn't needed. Therefore, clear it here.
        _registerCallbackArray.clear();
        return ok;
    }

    bool ScriptEngine::isGarbageCollecting()
    {
        return _isGarbageCollecting;
    }

    void ScriptEngine::_setGarbageCollecting(bool isGarbageCollecting)
    {
        _isGarbageCollecting = isGarbageCollecting;
    }

    void ScriptEngine::garbageCollect()
    {
        LOGD("GC begin ..., (Native -> JS map) count: %d\n", (int)NativePtrToObjectMap::size());
        _CHECK(JsCollectGarbage(_rt));
        LOGD("GC end ..., (Native -> JS map) count: %d\n", (int)NativePtrToObjectMap::size());
    }

    void ScriptEngine::clearException()
    {
        bool hasException = false;
        _CHECK(JsHasException(&hasException));

        if (hasException)
        {
            JsValueRef exception;
            _CHECK(JsGetAndClearException(&exception));

            ExceptionInfo exceptionInfo = formatException(exception);
            LOGD("ERROR: %s, %s, \nSTACK:\n%s\n", exceptionInfo.message.c_str(), exceptionInfo.location.c_str(), exceptionInfo.stack.c_str());

            if (_exceptionCallback != nullptr)
            {
                _exceptionCallback(exceptionInfo.location.c_str(), exceptionInfo.message.c_str(), exceptionInfo.stack.c_str());
            }

            if (!_isErrorHandleWorking)
            {
                _isErrorHandleWorking = true;

                Value errorHandler;
                if (_globalObj->getProperty("__errorHandler", &errorHandler) && errorHandler.isObject() && errorHandler.toObject()->isFunction())
                {
                    ValueArray args;
                    args.push_back(Value(exceptionInfo.location));
                    args.push_back(Value(0));
                    args.push_back(Value(exceptionInfo.message));
                    args.push_back(Value(exceptionInfo.stack));
                    errorHandler.toObject()->call(args, _globalObj);
                }

                _isErrorHandleWorking = false;
            }
            else
            {
                LOGE("ERROR: __errorHandler has exception\n");
            }
        }
    }

    void ScriptEngine::setExceptionCallback(const ExceptionCallback& cb)
    {
        _exceptionCallback = cb;
    }

    bool ScriptEngine::evalString(const char* script, ssize_t length/* = -1 */, Value* ret/* = nullptr */, const char* fileName/* = nullptr */)
    {
        assert(script != nullptr);
        if (length < 0)
            length = strlen(script);

        if (fileName == nullptr)
            fileName = "(no filename)";

        JsValueRef fname;
        _CHECK(JsCreateString(fileName, strlen(fileName), &fname));

        JsValueRef scriptSource;
        _CHECK(JsCreateString(script, length, &scriptSource));

        JsValueRef result;
        // Run the script.
        JsErrorCode errCode = JsRun(scriptSource, _currentSourceContext++, fname, JsParseScriptAttributeNone, &result);

        if (errCode != JsNoError)
        {
            clearException();
            return false;
        }

        if (ret != nullptr)
        {
            JsValueType type;
            JsGetValueType(result, &type);
            if (type != JsUndefined)
            {
                internal::jsToSeValue(result, ret);
            }
            else
            {
                ret->setUndefined();
            }
        }

        return true;
    }

    void ScriptEngine::setFileOperationDelegate(const FileOperationDelegate& delegate)
    {
        _fileOperationDelegate = delegate;
    }

    bool ScriptEngine::runScript(const std::string& path, Value* ret/* = nullptr */)
    {
        assert(!path.empty());
        assert(_fileOperationDelegate.isValid());

        std::string scriptBuffer = _fileOperationDelegate.onGetStringFromFile(path);

        if (!scriptBuffer.empty())
        {
            return evalString(scriptBuffer.c_str(), scriptBuffer.length(), ret, path.c_str());
        }

        LOGE("ScriptEngine::runScript script buffer is empty!\n");
        return false;
    }

    void ScriptEngine::_retainScriptObject(void* owner, void* target)
    {
        auto iterOwner = NativePtrToObjectMap::find(owner);
        if (iterOwner == NativePtrToObjectMap::end())
        {
            return;
        }

        auto iterTarget = NativePtrToObjectMap::find(target);
        if (iterTarget == NativePtrToObjectMap::end())
        {
            return;
        }

        clearException();
        iterOwner->second->attachObject(iterTarget->second);
    }

    void ScriptEngine::_releaseScriptObject(void* owner, void* target)
    {
        auto iterOwner = NativePtrToObjectMap::find(owner);
        if (iterOwner == NativePtrToObjectMap::end())
        {
            return;
        }

        auto iterTarget = NativePtrToObjectMap::find(target);
        if (iterTarget == NativePtrToObjectMap::end())
        {
            return;
        }

        clearException();
        iterOwner->second->detachObject(iterTarget->second);
    }

    void ScriptEngine::enableDebugger(const std::string& serverAddr, uint32_t port)
    {
        //FIXME:
    }

    bool ScriptEngine::isDebuggerEnabled() const
    {
        //FIXME:
        return false;
    }

    void ScriptEngine::mainLoopUpdate()
    {
        //FIXME:
    }

} // namespace se {

#endif // #if SCRIPT_ENGINE_TYPE == SCRIPT_ENGINE_CHAKRACORE
