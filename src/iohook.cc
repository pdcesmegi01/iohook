#include "iohook.h"

#include "uiohook.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <napi.h>

#ifdef _WIN32
#include <windows.h>
#else
#if defined(__APPLE__) && defined(__MACH__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <pthread.h>
#endif

static std::atomic<bool> sIsRunning(false);
static bool sIsDebug = false;

static std::thread sRunThread;
static std::mutex sTsfnMutex;
static bool sTsfnActive = false;
static Napi::ThreadSafeFunction sThreadSafeFunction;

// Native thread errors.
#define UIOHOOK_ERROR_THREAD_CREATE 0x10

// Thread and mutex variables.
#ifdef _WIN32
static HANDLE hook_thread;

static CRITICAL_SECTION hook_running_mutex;
static CRITICAL_SECTION hook_control_mutex;
static CONDITION_VARIABLE hook_control_cond;
#else
static pthread_t hook_thread;

static pthread_mutex_t hook_running_mutex;
static pthread_mutex_t hook_control_mutex;
static pthread_cond_t hook_control_cond;
#endif

static bool logger_proc(unsigned int level, const char *format, ...) {
  if (!sIsDebug) {
    return false;
  }

  bool status = false;
  va_list args;
  switch (level) {
    case LOG_LEVEL_DEBUG:
    case LOG_LEVEL_INFO:
      va_start(args, format);
      status = vfprintf(stdout, format, args) >= 0;
      va_end(args);
      break;

    case LOG_LEVEL_WARN:
    case LOG_LEVEL_ERROR:
      va_start(args, format);
      status = vfprintf(stderr, format, args) >= 0;
      va_end(args);
      break;
  }

  return status;
}

static NativeEventData ConvertNativeEvent(const uiohook_event &event) {
  NativeEventData out{};
  out.type = static_cast<uint16_t>(event.type);
  out.mask = static_cast<uint16_t>(event.mask);
  out.time = static_cast<uint64_t>(event.time);

  const bool isKeyboard = (event.type >= EVENT_KEY_TYPED) && (event.type <= EVENT_KEY_RELEASED);
  const bool isMouse = (event.type >= EVENT_MOUSE_CLICKED) && (event.type < EVENT_MOUSE_WHEEL);
  const bool isWheel = (event.type == EVENT_MOUSE_WHEEL);

  out.hasKeyboard = isKeyboard;
  out.hasMouse = isMouse;
  out.hasWheel = isWheel;

  if (isKeyboard) {
    out.keycode = static_cast<uint16_t>(event.data.keyboard.keycode);
    out.rawcode = static_cast<uint16_t>(event.data.keyboard.rawcode);
    out.keychar = static_cast<uint16_t>(event.data.keyboard.keychar);
  }

  if (isMouse) {
    out.button = static_cast<uint16_t>(event.data.mouse.button);
    out.clicks = static_cast<uint16_t>(event.data.mouse.clicks);
    out.mouseX = static_cast<int16_t>(event.data.mouse.x);
    out.mouseY = static_cast<int16_t>(event.data.mouse.y);
  }

  if (isWheel) {
    out.amount = static_cast<uint16_t>(event.data.wheel.amount);
    out.wheelClicks = static_cast<uint16_t>(event.data.wheel.clicks);
    out.direction = static_cast<int16_t>(event.data.wheel.direction);
    out.rotation = static_cast<int16_t>(event.data.wheel.rotation);
    out.wheelType = static_cast<int16_t>(event.data.wheel.type);
    out.wheelX = static_cast<int16_t>(event.data.wheel.x);
    out.wheelY = static_cast<int16_t>(event.data.wheel.y);
  }

  return out;
}

static Napi::Object FillEventObject(Napi::Env env, const NativeEventData &event) {
  Napi::Object obj = Napi::Object::New(env);
  obj.Set("type", Napi::Number::New(env, event.type));
  obj.Set("mask", Napi::Number::New(env, event.mask));
  obj.Set("time", Napi::Number::New(env, static_cast<double>(event.time)));

  if (event.hasKeyboard) {
    Napi::Object keyboard = Napi::Object::New(env);

    keyboard.Set("shiftKey", Napi::Boolean::New(env, event.keycode == VC_SHIFT_L || event.keycode == VC_SHIFT_R));
    keyboard.Set("altKey", Napi::Boolean::New(env, event.keycode == VC_ALT_L || event.keycode == VC_ALT_R));
    keyboard.Set("ctrlKey", Napi::Boolean::New(env, event.keycode == VC_CONTROL_L || event.keycode == VC_CONTROL_R));
    keyboard.Set("metaKey", Napi::Boolean::New(env, event.keycode == VC_META_L || event.keycode == VC_META_R));

    if (event.type == EVENT_KEY_TYPED) {
      keyboard.Set("keychar", Napi::Number::New(env, event.keychar));
    }

    keyboard.Set("keycode", Napi::Number::New(env, event.keycode));
    keyboard.Set("rawcode", Napi::Number::New(env, event.rawcode));
    obj.Set("keyboard", keyboard);
  } else if (event.hasMouse) {
    Napi::Object mouse = Napi::Object::New(env);
    mouse.Set("button", Napi::Number::New(env, event.button));
    mouse.Set("clicks", Napi::Number::New(env, event.clicks));
    mouse.Set("x", Napi::Number::New(env, event.mouseX));
    mouse.Set("y", Napi::Number::New(env, event.mouseY));
    obj.Set("mouse", mouse);
  } else if (event.hasWheel) {
    Napi::Object wheel = Napi::Object::New(env);
    wheel.Set("amount", Napi::Number::New(env, event.amount));
    wheel.Set("clicks", Napi::Number::New(env, event.wheelClicks));
    wheel.Set("direction", Napi::Number::New(env, event.direction));
    wheel.Set("rotation", Napi::Number::New(env, event.rotation));
    wheel.Set("type", Napi::Number::New(env, event.wheelType));
    wheel.Set("x", Napi::Number::New(env, event.wheelX));
    wheel.Set("y", Napi::Number::New(env, event.wheelY));
    obj.Set("wheel", wheel);
  }

  return obj;
}

// NOTE: The following callback executes on the same thread that hook_run() is called
// from.
static void dispatch_proc(uiohook_event *const event) {
  switch (event->type) {
    case EVENT_HOOK_ENABLED:
#ifdef _WIN32
      EnterCriticalSection(&hook_running_mutex);
      WakeConditionVariable(&hook_control_cond);
      LeaveCriticalSection(&hook_control_mutex);
#else
      pthread_mutex_lock(&hook_running_mutex);
      pthread_cond_signal(&hook_control_cond);
      pthread_mutex_unlock(&hook_control_mutex);
#endif
      break;

    case EVENT_HOOK_DISABLED:
#ifdef _WIN32
      EnterCriticalSection(&hook_control_mutex);
      LeaveCriticalSection(&hook_running_mutex);
#else
      pthread_mutex_lock(&hook_control_mutex);
#if defined(__APPLE__) && defined(__MACH__)
      CFRunLoopStop(CFRunLoopGetMain());
#endif
      pthread_mutex_unlock(&hook_running_mutex);
#endif
      break;

    case EVENT_KEY_PRESSED:
    case EVENT_KEY_RELEASED:
    case EVENT_KEY_TYPED:
    case EVENT_MOUSE_PRESSED:
    case EVENT_MOUSE_RELEASED:
    case EVENT_MOUSE_CLICKED:
    case EVENT_MOUSE_MOVED:
    case EVENT_MOUSE_DRAGGED:
    case EVENT_MOUSE_WHEEL: {
      auto nativeEvent = std::make_unique<NativeEventData>(ConvertNativeEvent(*event));

      std::lock_guard<std::mutex> lock(sTsfnMutex);
      if (!sTsfnActive) {
        break;
      }

      NativeEventData *rawEvent = nativeEvent.release();
      const napi_status status = sThreadSafeFunction.NonBlockingCall(
          rawEvent,
          [](Napi::Env env, Napi::Function jsCallback, NativeEventData *data) {
            std::unique_ptr<NativeEventData> guard(data);
            Napi::Object obj = FillEventObject(env, *data);
            jsCallback.Call({obj});
          });

      if (status != napi_ok) {
        delete rawEvent;
      }
      break;
    }
  }
}

#ifdef _WIN32
static DWORD WINAPI hook_thread_proc(LPVOID arg) {
#else
static void *hook_thread_proc(void *arg) {
#endif
  int status = hook_run();
  if (status != UIOHOOK_SUCCESS) {
#ifdef _WIN32
    *(DWORD *)arg = status;
#else
    *(int *)arg = status;
#endif
  }

#ifdef _WIN32
  WakeConditionVariable(&hook_control_cond);
  LeaveCriticalSection(&hook_control_mutex);
  return status;
#else
  pthread_cond_signal(&hook_control_cond);
  pthread_mutex_unlock(&hook_control_mutex);
  return arg;
#endif
}

static int hook_enable() {
#ifdef _WIN32
  EnterCriticalSection(&hook_control_mutex);
#else
  pthread_mutex_lock(&hook_control_mutex);
#endif

  int status = UIOHOOK_FAILURE;

#ifndef _WIN32
  pthread_attr_t hook_thread_attr;
  pthread_attr_init(&hook_thread_attr);

  int policy;
  pthread_attr_getschedpolicy(&hook_thread_attr, &policy);
  int priority = sched_get_priority_max(policy);
#endif

#if defined(_WIN32)
  DWORD hook_thread_id;
  DWORD *hook_thread_status = (DWORD *)malloc(sizeof(DWORD));
  hook_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)hook_thread_proc, hook_thread_status, 0, &hook_thread_id);
  if (hook_thread != INVALID_HANDLE_VALUE) {
#else
  int *hook_thread_status = (int *)malloc(sizeof(int));
  if (pthread_create(&hook_thread, &hook_thread_attr, hook_thread_proc, hook_thread_status) == 0) {
#endif
#if defined(_WIN32)
    if (SetThreadPriority(hook_thread, THREAD_PRIORITY_TIME_CRITICAL) == 0) {
      logger_proc(LOG_LEVEL_WARN,
                  "%s [%u]: Could not set thread priority %li for thread %#p! (%#lX)\n",
                  __FUNCTION__, __LINE__, (long)THREAD_PRIORITY_TIME_CRITICAL,
                  hook_thread, (unsigned long)GetLastError());
    }
#elif (defined(__APPLE__) && defined(__MACH__)) || _POSIX_C_SOURCE >= 200112L
    struct sched_param param = {.sched_priority = priority};
    if (pthread_setschedparam(hook_thread, SCHED_OTHER, &param) != 0) {
      logger_proc(LOG_LEVEL_WARN,
                  "%s [%u]: Could not set thread priority %i for thread 0x%lX!\n",
                  __FUNCTION__, __LINE__, priority, (unsigned long)hook_thread);
    }
#else
    if (pthread_setschedprio(hook_thread, priority) != 0) {
      logger_proc(LOG_LEVEL_WARN,
                  "%s [%u]: Could not set thread priority %i for thread 0x%lX!\n",
                  __FUNCTION__, __LINE__, priority, (unsigned long)hook_thread);
    }
#endif

#ifdef _WIN32
    SleepConditionVariableCS(&hook_control_cond, &hook_control_mutex, INFINITE);
#else
    pthread_cond_wait(&hook_control_cond, &hook_control_mutex);
#endif

#ifdef _WIN32
    if (TryEnterCriticalSection(&hook_running_mutex) != FALSE) {
#else
    if (pthread_mutex_trylock(&hook_running_mutex) == 0) {
#endif
#ifdef _WIN32
      WaitForSingleObject(hook_thread, INFINITE);
      GetExitCodeThread(hook_thread, hook_thread_status);
      status = static_cast<int>(*hook_thread_status);
#else
      pthread_join(hook_thread, (void **)&hook_thread_status);
      status = *hook_thread_status;
#endif
    } else {
      status = UIOHOOK_SUCCESS;
    }

    free(hook_thread_status);

    logger_proc(LOG_LEVEL_DEBUG, "%s [%u]: Thread Result: (%#X).\n", __FUNCTION__, __LINE__, status);
  } else {
    status = UIOHOOK_ERROR_THREAD_CREATE;
  }

#ifdef _WIN32
  LeaveCriticalSection(&hook_control_mutex);
#else
  pthread_mutex_unlock(&hook_control_mutex);
#endif

  return status;
}

static void run() {
#ifdef _WIN32
  InitializeCriticalSection(&hook_running_mutex);
  InitializeCriticalSection(&hook_control_mutex);
  InitializeConditionVariable(&hook_control_cond);
#else
  pthread_mutex_init(&hook_running_mutex, NULL);
  pthread_mutex_init(&hook_control_mutex, NULL);
  pthread_cond_init(&hook_control_cond, NULL);
#endif

  hook_set_logger_proc(&logger_proc);
  hook_set_dispatch_proc(&dispatch_proc);

  int status = hook_enable();
  switch (status) {
    case UIOHOOK_SUCCESS:
#ifdef _WIN32
      WaitForSingleObject(hook_thread, INFINITE);
#else
#if defined(__APPLE__) && defined(__MACH__)
      CFRunLoopRun();
#endif
      pthread_join(hook_thread, NULL);
#endif
      break;

    case UIOHOOK_ERROR_OUT_OF_MEMORY:
      logger_proc(LOG_LEVEL_ERROR, "Failed to allocate memory. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_OPEN_DISPLAY:
      logger_proc(LOG_LEVEL_ERROR, "Failed to open X11 display. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_NOT_FOUND:
      logger_proc(LOG_LEVEL_ERROR, "Unable to locate XRecord extension. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_ALLOC_RANGE:
      logger_proc(LOG_LEVEL_ERROR, "Unable to allocate XRecord range. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_CREATE_CONTEXT:
      logger_proc(LOG_LEVEL_ERROR, "Unable to allocate XRecord context. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_ENABLE_CONTEXT:
      logger_proc(LOG_LEVEL_ERROR, "Failed to enable XRecord context. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_SET_WINDOWS_HOOK_EX:
      logger_proc(LOG_LEVEL_ERROR, "Failed to register low level windows hook. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_AXAPI_DISABLED:
      logger_proc(LOG_LEVEL_ERROR, "Failed to enable access for assistive devices. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_CREATE_EVENT_PORT:
      logger_proc(LOG_LEVEL_ERROR, "Failed to create apple event port. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_CREATE_RUN_LOOP_SOURCE:
      logger_proc(LOG_LEVEL_ERROR, "Failed to create apple run loop source. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_GET_RUNLOOP:
      logger_proc(LOG_LEVEL_ERROR, "Failed to acquire apple run loop. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_CREATE_OBSERVER:
      logger_proc(LOG_LEVEL_ERROR, "Failed to create apple run loop observer. (%#X)\n", status);
      break;

    case UIOHOOK_FAILURE:
    default:
      logger_proc(LOG_LEVEL_ERROR, "An unknown hook error occurred. (%#X)\n", status);
      break;
  }
}

static void stop() {
  int status = hook_stop();
  switch (status) {
    case UIOHOOK_ERROR_OUT_OF_MEMORY:
      logger_proc(LOG_LEVEL_ERROR, "Failed to allocate memory. (%#X)", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_GET_CONTEXT:
      logger_proc(LOG_LEVEL_ERROR, "Failed to get XRecord context. (%#X)", status);
      break;

    case UIOHOOK_FAILURE:
    default:
      logger_proc(LOG_LEVEL_ERROR, "An unknown hook error occurred. (%#X)", status);
      break;
  }

#ifdef _WIN32
  CloseHandle(hook_thread);
  DeleteCriticalSection(&hook_running_mutex);
  DeleteCriticalSection(&hook_control_mutex);
#else
  pthread_mutex_destroy(&hook_running_mutex);
  pthread_mutex_destroy(&hook_control_mutex);
  pthread_cond_destroy(&hook_control_cond);
#endif
}

static void GrabMouseClick(const Napi::CallbackInfo &info) {
  if (info.Length() > 0 && info[0].IsBoolean()) {
    grab_mouse_click(info[0].As<Napi::Boolean>().Value());
  }
}

static void GrabKeyboard(const Napi::CallbackInfo &info) {
  if (info.Length() > 0 && info[0].IsBoolean()) {
    grab_keyboard(info[0].As<Napi::Boolean>().Value());
  }
}

static void DebugEnable(const Napi::CallbackInfo &info) {
  if (info.Length() > 0 && info[0].IsBoolean()) {
    sIsDebug = info[0].As<Napi::Boolean>().Value();
  }
}

static void ReleaseThreadSafeFunction() {
  std::lock_guard<std::mutex> lock(sTsfnMutex);
  if (sTsfnActive) {
    sThreadSafeFunction.Release();
    sTsfnActive = false;
    sThreadSafeFunction = Napi::ThreadSafeFunction();
  }
}

static void StartHook(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (sIsRunning.load()) {
    return;
  }

  if (info.Length() == 0 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "startHook requires a callback function").ThrowAsJavaScriptException();
    return;
  }

  if (info.Length() == 2 && info[1].IsBoolean()) {
    sIsDebug = info[1].As<Napi::Boolean>().Value();
  }

  Napi::Function callback = info[0].As<Napi::Function>();
  {
    std::lock_guard<std::mutex> lock(sTsfnMutex);
    sThreadSafeFunction = Napi::ThreadSafeFunction::New(
        env,
        callback,
        "iohook-event-dispatch",
        1024,
        1);
    sTsfnActive = true;
  }

  sIsRunning.store(true);
  sRunThread = std::thread([]() {
    run();
    sIsRunning.store(false);
    ReleaseThreadSafeFunction();
  });
}

static void StopHook(const Napi::CallbackInfo &info) {
  (void)info;

  if (sIsRunning.load()) {
    stop();
  }

  if (sRunThread.joinable()) {
    sRunThread.join();
  }

  sIsRunning.store(false);
  ReleaseThreadSafeFunction();
}

static void CleanupHook() {
  if (sIsRunning.load()) {
    stop();
  }
  if (sRunThread.joinable()) {
    sRunThread.join();
  }

  sIsRunning.store(false);
  ReleaseThreadSafeFunction();
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
  env.AddCleanupHook(CleanupHook);

  exports.Set("startHook", Napi::Function::New(env, StartHook));
  exports.Set("stopHook", Napi::Function::New(env, StopHook));
  exports.Set("debugEnable", Napi::Function::New(env, DebugEnable));
  exports.Set("grabMouseClick", Napi::Function::New(env, GrabMouseClick));
  exports.Set("grabKeyboard", Napi::Function::New(env, GrabKeyboard));
  return exports;
}

NODE_API_MODULE(iohook, Init)
