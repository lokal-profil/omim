//----------------------------------------------------------------------------------
// File:            libs\jni\nv_event\nv_event.cpp
// Samples Version: NVIDIA Android Lifecycle samples 1_0beta
// Email:           tegradev@nvidia.com
// Web:             http://developer.nvidia.com/category/zone/mobile-development
//
// Copyright 2009-2011 NVIDIA� Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//----------------------------------------------------------------------------------

#define MODULE "NVEvent"
#define DBG_DETAILED 0

#include "nv_event.hpp"

#include <stdlib.h>
#include <jni.h>
#include <pthread.h>
#include <android/log.h>
#include <GLES2/gl2.h>
//#include <EGL/egl.h>

#include <mutex>

#include "../nv_time/nv_time.hpp"
#include "../nv_thread/nv_thread.hpp"
#include "../nv_debug/nv_debug.hpp"
#include "scoped_profiler.hpp"
#include "nv_keycode_mapping.hpp"
#include "nv_event_queue.hpp"

#define CT_ASSERT(tag,cond) \
enum { COMPILE_TIME_ASSERT__ ## tag = 1/(cond) }

enum
{
  // Android lifecycle status flags.  Not app-specific
  // Set between onCreate and onDestroy
  NVEVENT_STATUS_RUNNING          = 0x00000001,
  // Set between onResume and onPause
  NVEVENT_STATUS_ACTIVE           = 0x00000002,
  // Set between onWindowFocusChanged(true) and (false)
  NVEVENT_STATUS_FOCUSED          = 0x00000004,
  // Set when the app's SurfaceHolder points to a
  // valid, nonzero-sized surface
  NVEVENT_STATUS_HAS_REAL_SURFACE  = 0x00000008,

  // Mask of all app lifecycle status flags, useful for checking when is it
  // a reasonable time to be setting up EGL and rendering
  NVEVENT_STATUS_INTERACTABLE     = 0x0000000f,

  // NvEvent EGL status flags.  Not app-specific
  // Set between calls to NVEventInitEGL and NVEventCleanupEGL
  NVEVENT_STATUS_EGL_INITIALIZED  = 0x00000010,
  // Set when the EGL surface is allocated
  NVEVENT_STATUS_EGL_HAS_SURFACE  = 0x00000020,
  // Set when a surface and context are available and bound
  NVEVENT_STATUS_EGL_BOUND        = 0x00000040,
};

static unsigned int s_appStatus = 0;

static void ZeroAppFlags()
{
  s_appStatus = 0;
}

static void SetAppFlag(unsigned int status)
{
  s_appStatus |= status;
}

static void ClearAppFlag(unsigned int status)
{
  s_appStatus &= ~status;
}

static bool QueryAppFlag(unsigned int status)
{
  return (s_appStatus & status) ? true : false;
}

static bool QueryAppFlagsEqualMasked(unsigned int status, unsigned int mask)
{
  return ((s_appStatus & mask) == status) ? true : false;
}

static NVKeyCodeMapping s_keyMapping;
static NVEventQueue s_eventQueue;
static jobject s_globalThiz;
static jfieldID s_lengthId;
static jfieldID s_dataId;
static jfieldID s_widthId;
static jfieldID s_heightId;
static jfieldID s_texDataId;
static pthread_t   s_mainThread;
static bool s_appThreadExited = false;
static bool s_javaPostedQuit = false;

static int NVEVENT_ACTION_DOWN = 0;
static int NVEVENT_ACTION_UP = 0;
static int NVEVENT_ACTION_CANCEL = 0;
static int NVEVENT_ACTION_POINTER_INDEX_MASK = 0;
static int NVEVENT_ACTION_POINTER_INDEX_SHIFT = 0;
static int NVEVENT_ACTION_KEY_UP = 0;

class MethodRef
{
public:
  MethodRef(const char* name,
            const char* signature) :
    m_name(name),
    m_signature(signature),
    m_index(NULL)
  {}

  bool QueryID(JNIEnv* env, jclass k)
  {
    m_index = env->GetMethodID(k, m_name, m_signature);
    return (0 != m_index);
  }

  bool CallBoolean() const
  {
    JNIEnv* const jniEnv = NVThreadGetCurrentJNIEnv();

    if (!jniEnv || !s_globalThiz)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid JNI env in %s", m_name);
      return false;
    }

    if (!m_index)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid function pointer in %s", m_name);
      return false;
    }

    return jniEnv->CallBooleanMethod(s_globalThiz, m_index);
  }

  template <typename P1, typename P2>
  bool CallBoolean(P1 p1, P2 p2) const
  {
    JNIEnv* const jniEnv = NVThreadGetCurrentJNIEnv();

    if (!jniEnv || !s_globalThiz)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid JNI env in %s", m_name);
      return false;
    }
    if (!m_index)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid function pointer in %s", m_name);
      return false;
    }

    return jniEnv->CallBooleanMethod(s_globalThiz, m_index, p1, p2);
  }

  int CallInt() const
  {
    JNIEnv* const jniEnv = NVThreadGetCurrentJNIEnv();

    if (!jniEnv || !s_globalThiz)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid JNI env in %s", m_name);
      return 0;
    }

    if (!m_index)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid function pointer in %s", m_name);
      return 0;
    }

    return jniEnv->CallIntMethod(s_globalThiz, m_index);
  }

  void CallVoid() const
  {
    JNIEnv * const jniEnv = NVThreadGetCurrentJNIEnv();

    if (!jniEnv || !s_globalThiz)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid JNI env in %s", m_name);
      return;
    }

    if (!m_index)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: No valid function pointer in %s", m_name);
      return;
    }

    jniEnv->CallVoidMethod(s_globalThiz, m_index);
  }

private:
  const char* const m_name;
  const char* const m_signature;
  jmethodID m_index;
};

static MethodRef s_InitEGL("InitEGL", "()Z");
static MethodRef s_CleanupEGL("CleanupEGL", "()Z");
static MethodRef s_CreateSurfaceEGL("CreateSurfaceEGL", "()Z");
static MethodRef s_CreateOffScreenSurfaceEGL("CreateOffScreenSurfaceEGL", "(II)Z");
static MethodRef s_DestroySurfaceEGL("DestroySurfaceEGL", "()Z");
static MethodRef s_SwapBuffersEGL("SwapBuffersEGL", "()Z");
static MethodRef s_BindSurfaceAndContextEGL("BindSurfaceAndContextEGL", "()Z");
static MethodRef s_UnbindSurfaceAndContextEGL("UnbindSurfaceAndContextEGL", "()Z");
static MethodRef s_GetErrorEGL("GetErrorEGL", "()I");
static MethodRef s_finish("finish", "()V");
static MethodRef s_ReportUnsupported("ReportUnsupported", "()V");
static MethodRef s_OnRenderingInitialized("OnRenderingInitialized", "()V");

// True between onCreate and onDestroy
bool NVEventStatusIsRunning()
{
  // TBD - need to lock a mutex?
  return QueryAppFlag(NVEVENT_STATUS_RUNNING);
}

// True between onResume and onPause
bool NVEventStatusIsActive()
{
  // TBD - need to lock a mutex?
  return QueryAppFlag(NVEVENT_STATUS_ACTIVE);
}

// True between onWindowFocusChanged(true) and (false)
bool NVEventStatusIsFocused()
{
  // TBD - need to lock a mutex?
  return QueryAppFlag(NVEVENT_STATUS_FOCUSED);
}

// True when the app's SurfaceHolder points to a
// valid, nonzero-sized window
bool NVEventStatusHasRealSurface()
{
  // TBD - need to lock a mutex?
  return QueryAppFlag(NVEVENT_STATUS_HAS_REAL_SURFACE);
}

// True when all of IsRunning, IsActive, IsFocused, HasRealSurface are true
// useful for checking when is it a reasonable time to be setting up EGL and rendering
bool NVEventStatusIsInteractable()
{
  // TBD - need to lock a mutex?
  return QueryAppFlagsEqualMasked(NVEVENT_STATUS_INTERACTABLE, NVEVENT_STATUS_INTERACTABLE);
}

// True between calls to NVEventInitEGL and NVEventCleanupEGL
bool NVEventStatusEGLInitialized()
{
  // TBD - need to lock a mutex?
  return QueryAppFlag(NVEVENT_STATUS_EGL_INITIALIZED);
}

// True when the EGL surface is allocated
bool NVEventStatusEGLHasSurface()
{
  // TBD - need to lock a mutex?
  return QueryAppFlag(NVEVENT_STATUS_EGL_HAS_SURFACE);
}

// True when a surface and context are available and bound
bool NVEventStatusEGLIsBound()
{
  // TBD - need to lock a mutex?
  return QueryAppFlag(NVEVENT_STATUS_EGL_BOUND);
}

static void NVEventInitInputFields(JNIEnv *env)
{
  jclass const Motion_class = env->FindClass("android/view/MotionEvent");
  jfieldID const ACTION_DOWN_id = env->GetStaticFieldID(Motion_class, "ACTION_DOWN", "I");
  jfieldID const ACTION_UP_id = env->GetStaticFieldID(Motion_class, "ACTION_UP", "I");
  jfieldID const ACTION_CANCEL_id = env->GetStaticFieldID(Motion_class, "ACTION_CANCEL", "I");
  jfieldID const ACTION_POINTER_INDEX_SHIFT_id = env->GetStaticFieldID(Motion_class, "ACTION_POINTER_ID_SHIFT", "I");
  jfieldID const ACTION_POINTER_INDEX_MASK_id = env->GetStaticFieldID(Motion_class, "ACTION_POINTER_ID_MASK", "I");

  NVEVENT_ACTION_DOWN = env->GetStaticIntField(Motion_class, ACTION_DOWN_id);
  NVEVENT_ACTION_UP = env->GetStaticIntField(Motion_class, ACTION_UP_id);
  NVEVENT_ACTION_CANCEL = env->GetStaticIntField(Motion_class, ACTION_CANCEL_id);
  NVEVENT_ACTION_POINTER_INDEX_MASK = env->GetStaticIntField(Motion_class, ACTION_POINTER_INDEX_MASK_id);
  NVEVENT_ACTION_POINTER_INDEX_SHIFT = env->GetStaticIntField(Motion_class, ACTION_POINTER_INDEX_SHIFT_id);

  jclass const KeyCode_class = env->FindClass("android/view/KeyEvent");
  jfieldID const ACTION_KEY_UP_id = env->GetStaticFieldID(KeyCode_class, "ACTION_UP", "I");

  NVEVENT_ACTION_KEY_UP = env->GetStaticIntField(KeyCode_class, ACTION_KEY_UP_id);
}

static void* NVEventMainLoopThreadFunc(void*)
{
  NVEventAppMain(0, NULL);

  __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "NvEvent native app Main returned");

  // signal the condition variable to unblock
  // java from waiting on pause or quit
  s_eventQueue.UnblockProducer();

  s_appThreadExited = true;

  // IF that app main returned because we posted a QUIT, then Java knows what to
  // do regarding lifecycle.  But, if the app returned from main of its own accord,
  // we need to call finish.
  if (!s_javaPostedQuit)
  {
    s_finish.CallVoid();
  }

  return NULL;
}

NVEventPlatformAppHandle NVEventGetPlatformAppHandle()
{
  return s_globalThiz;
}

///////////////////////////////////////////////////////////////////////////////
// Native event-handling functions

const char* NVEventGetEventStr(NVEventType eventType)
{
  switch(eventType)
  {
    case NV_EVENT_KEY:              return "NV_EVENT_KEY";
    case NV_EVENT_CHAR:             return "NV_EVENT_CHAR";
    case NV_EVENT_TOUCH:            return "NV_EVENT_TOUCH";
    case NV_EVENT_MULTITOUCH:       return "NV_EVENT_MULTITOUCH";
    case NV_EVENT_ACCEL:            return "NV_EVENT_ACCEL";
    case NV_EVENT_START:            return "NV_EVENT_START";
    case NV_EVENT_RESTART:          return "NV_EVENT_RESTART";
    case NV_EVENT_RESUME:           return "NV_EVENT_RESUME";
    case NV_EVENT_FOCUS_GAINED:     return "NV_EVENT_FOCUS_GAINED";
    case NV_EVENT_SURFACE_CREATED:   return "NV_EVENT_SURFACE_CREATED";
    case NV_EVENT_SURFACE_SIZE:      return "NV_EVENT_SURFACE_SIZE";
    case NV_EVENT_SURFACE_DESTROYED: return "NV_EVENT_SURFACE_DESTROYED";
    case NV_EVENT_FOCUS_LOST:       return "NV_EVENT_FOCUS_LOST";
    case NV_EVENT_PAUSE:            return "NV_EVENT_PAUSE";
    case NV_EVENT_STOP:             return "NV_EVENT_STOP";
    case NV_EVENT_QUIT:             return "NV_EVENT_QUIT";
    case NV_EVENT_USER:             return "NV_EVENT_USER";
    case NV_EVENT_LONG_CLICK:       return "NV_EVENT_LONG_CLICK";
    case NV_EVENT_MWM:              return "NV_EVENT_MWM";
  }

  // update this if you end up having to edit something.
  CT_ASSERT(NEED_TO_ADD_STRING_HERE, NV_EVENT_NUM_EVENTS == 19);
  return "unknown event type!";
}

const NVEvent* NVEventGetNextEvent(int waitMSecs)
{
  return s_eventQueue.RemoveOldest(waitMSecs);
}

void NVEventDoneWithEvent(bool handled)
{
  return s_eventQueue.DoneWithEvent(handled);
}

static void NVEventInsert(NVEvent* ev)
{
  if(!s_appThreadExited)
    s_eventQueue.Insert(ev);
}

static bool NVEventInsertBlocking(NVEvent* ev)
{
  if(!s_appThreadExited)
    return s_eventQueue.InsertBlocking(ev);

  return false;
}

///////////////////////////////////////////////////////////////////////////////
// Native to Java EGL call-up functions

bool NVEventInitEGL()
{
  if(s_InitEGL.CallBoolean())
  {
    SetAppFlag(NVEVENT_STATUS_EGL_INITIALIZED);
    return true;
  }
  else
    return false;
}

bool NVEventCleanupEGL()
{
  ClearAppFlag(NVEVENT_STATUS_EGL_BOUND);
  ClearAppFlag(NVEVENT_STATUS_EGL_HAS_SURFACE);
  ClearAppFlag(NVEVENT_STATUS_EGL_INITIALIZED);
  return s_CleanupEGL.CallBoolean();
}

bool NVEventCreateSurfaceEGL()
{
  if (s_CreateSurfaceEGL.CallBoolean())
  {
    SetAppFlag(NVEVENT_STATUS_EGL_HAS_SURFACE);
    return true;
  }
  else
    return false;
}

bool NVEventCreateOffScreenSurfaceEGL(int width, int height)
{
  if (s_CreateOffScreenSurfaceEGL.CallBoolean(width, height))
  {
    SetAppFlag(NVEVENT_STATUS_EGL_HAS_SURFACE);
    return true;
  }
  else
    return false;
}

bool NVEventDestroySurfaceEGL()
{
  if (!QueryAppFlag(NVEVENT_STATUS_EGL_HAS_SURFACE))
    return true;

  if (QueryAppFlag(NVEVENT_STATUS_EGL_BOUND))
    NVEventUnbindSurfaceAndContextEGL();

  ClearAppFlag(NVEVENT_STATUS_EGL_HAS_SURFACE);
  return s_DestroySurfaceEGL.CallBoolean();
}

bool NVEventBindSurfaceAndContextEGL()
{
  if (s_BindSurfaceAndContextEGL.CallBoolean())
  {
    SetAppFlag(NVEVENT_STATUS_EGL_BOUND);
    return true;
  }
  else
    return false;
}

bool NVEventUnbindSurfaceAndContextEGL()
{
  ClearAppFlag(NVEVENT_STATUS_EGL_BOUND);
  return s_UnbindSurfaceAndContextEGL.CallBoolean();
}

bool NVEventSwapBuffersEGL()
{
  if (!s_SwapBuffersEGL.CallBoolean())
    return false;
  RESET_PROFILING();
  return true;
}

int NVEventGetErrorEGL()
{
  return s_GetErrorEGL.CallInt();
}

// TODO make it in normal way
// Trick: if surface width/height are specified then create off-screen surface
static int s_surfaceWidth = 0;
static int s_surfaceHeight = 0;
void PrepareWindowSurface()
{
  s_surfaceWidth = 0;
  s_surfaceHeight = 0;
}
void PrepareOffScreenSurface(int width, int height)
{
  s_surfaceWidth = width;
  s_surfaceHeight = height;
}
bool IsPreparedForOffscreen()
{
  return s_surfaceWidth != 0 && s_surfaceHeight != 0;
}
bool IsPreparedForWindow()
{
  return s_surfaceWidth == 0 && s_surfaceHeight == 0;
}

bool NVEventReadyToRenderEGL(bool allocateIfNeeded)
{
  // If we have a bound context and surface, then EGL is ready
  if (!NVEventStatusEGLIsBound())
  {
    if (!allocateIfNeeded)
    {
      NVDEBUG("NVEventReadyToRenderEGL.NVEventInitEGL failed, allocateIfNeeded=false");
      return false;
    }

    // If we have not bound the context and surface, do we even _have_ a surface?
    if (!NVEventStatusEGLHasSurface())
    {
      // No surface, so we need to check if EGL is set up at all
      if (!NVEventStatusEGLInitialized())
      {
        if (!NVEventInitEGL())
        {
          NVDEBUG("NVEventReadyToRenderEGL.NVEventInitEGL failed");
          return false;
        }
      }

      // TODO make it in normal way
      // Trick: if surface width/height are specified then create off-screen surface
      if (IsPreparedForWindow())
      {
        // Create the rendering surface now that we have a context
        if (!NVEventCreateSurfaceEGL())
        {
          NVDEBUG("NVEventReadyToRenderEGL.NVEventCreateSurfaceEGL failed");
          return false;
        }
      }
      else
      {
        // Create the OFF-SCREEN rendering surface now that we have a context
        if (!NVEventCreateOffScreenSurfaceEGL(s_surfaceWidth, s_surfaceHeight))
        {
          NVDEBUG("NVEventReadyToRenderEGL.NVEventCreateOffScreenSurfaceEGL failed");
          return false;
        }
      }
    }

    // We have a surface and context, so bind them
    if (!NVEventBindSurfaceAndContextEGL())
    {
      NVDEBUG("NVEventReadyToRenderEGL.NVEventBindSurfaceAndContextEGL failed");
      return false;
    }
  }

  return true;
}

bool NVEventRepaint()
{
  NVEvent ev;
  ev.m_type = NV_EVENT_USER;
  ev.m_data.m_user.m_u0 = 1;
  NVEventInsert(&ev);

  return true;
}

void NVEventReportUnsupported()
{
  /// to prevent from rendering
  ClearAppFlag(NVEVENT_STATUS_FOCUSED);
  s_ReportUnsupported.CallVoid();
}

void NVEventOnRenderingInitialized()
{
  s_OnRenderingInitialized.CallVoid();
}

///////////////////////////////////////////////////////////////////////////////
// Input event-related Java to Native callback functions


static jboolean NVEventMultiTouchEvent(JNIEnv*  env, jobject  thiz, jint action,
									   jboolean hasFirst, jboolean hasSecond, jint mx1, jint my1, jint mx2, jint my2)
{
  {
    NVEvent ev;

    int const actionOnly = action & (~NVEVENT_ACTION_POINTER_INDEX_MASK);

    int maskOnly = 0;

    if (hasFirst)
      maskOnly |= 0x1;
    if (hasSecond)
      maskOnly |= 0x2;

    ev.m_type = NV_EVENT_MULTITOUCH;

    if (actionOnly == NVEVENT_ACTION_UP)
    {
      ev.m_data.m_multi.m_action = NV_MULTITOUCH_UP;
    }
    else if (actionOnly == NVEVENT_ACTION_DOWN)
    {
      ev.m_data.m_multi.m_action = NV_MULTITOUCH_DOWN;
    }
    else if (actionOnly == NVEVENT_ACTION_CANCEL)
    {
      ev.m_data.m_multi.m_action = NV_MULTITOUCH_CANCEL;
    }
    else
    {
      ev.m_data.m_multi.m_action = NV_MULTITOUCH_MOVE;
    }
    ev.m_data.m_multi.m_action =
        (NVMultiTouchEventType)(ev.m_data.m_multi.m_action | (maskOnly << NV_MULTITOUCH_POINTER_SHIFT));
    ev.m_data.m_multi.m_x1 = mx1;
    ev.m_data.m_multi.m_y1 = my1;
    ev.m_data.m_multi.m_x2 = mx2;
    ev.m_data.m_multi.m_y2 = my2;
    NVEventInsert(&ev);
  }

  return JNI_TRUE;
}

static jboolean NVEventKeyEvent(JNIEnv* env, jobject thiz, jint action, jint keycode, jint unichar)
{
  // TBD - remove these or make them resettable for safety...
  static int lastKeyAction = 0;
  static int lastKeyCode = 0;
  bool ret = false;

  NVKeyCode code = NV_KEYCODE_NULL;

  if (s_keyMapping.MapKey((int)keycode, code))
  {
    if ((code != NV_KEYCODE_NULL) &&
       ((code != lastKeyCode) || (action != lastKeyAction)))
    {
      NVEvent ev;
      ev.m_type = NV_EVENT_KEY;
      ev.m_data.m_key.m_action = (NVEVENT_ACTION_UP == action)
		        ? NV_KEYACTION_UP : NV_KEYACTION_DOWN;
      ev.m_data.m_key.m_code = code;
      ret = NVEventInsertBlocking(&ev);
    }

    lastKeyAction = action;
    lastKeyCode = code;
  }

  if (unichar && (NVEVENT_ACTION_UP != action))
  {
    NVEvent ev;
    ev.m_type = NV_EVENT_CHAR;
    ev.m_data.m_char.m_unichar = unichar;
    NVEventInsert(&ev);
  }

  return ret;
}

static jboolean NVEventAccelerometerEvent(JNIEnv* env, jobject thiz, jfloat values0, jfloat values1, jfloat values2)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_ACCEL;
  ev.m_data.m_accel.m_x = values0;
  ev.m_data.m_accel.m_y = values1;
  ev.m_data.m_accel.m_z = values2;
  NVEventInsert(&ev);
  return JNI_TRUE;
}

///////////////////////////////////////////////////////////////////////////////
// Java to Native app lifecycle callback functions

static std::mutex s_nativeConstructionMutex;
static size_t s_nativeRefCount = 0;

static jboolean onDestroyNative(JNIEnv* env, jobject thiz);

static jboolean onCreateNative(JNIEnv* env, jobject thiz)
{
  std::lock_guard<std::mutex> const l(s_nativeConstructionMutex);

  if (s_nativeRefCount != 0)
  {
    ++s_nativeRefCount;

    __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Native has already been created");
    return JNI_TRUE;
  }

  if (s_globalThiz)
    onDestroyNative(env, thiz);

  ZeroAppFlags();

  if (!s_globalThiz)
  {
    s_globalThiz = env->NewGlobalRef(thiz);
    if (!s_globalThiz)
    {
      __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: Thiz NewGlobalRef failed!");
    }

    __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Thiz NewGlobalRef: 0x%p", s_globalThiz);
  }

  __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Init KeyCode Map");
  s_keyMapping.Init(env, thiz);

  NVEventInitInputFields(env);

  s_eventQueue.Init();

  s_javaPostedQuit = false;

  __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Calling NVEventAppInit");

  if (0 != NVEventAppInit(0, NULL))
  {
    env->DeleteGlobalRef(s_globalThiz);
    s_globalThiz = NULL;

    __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "NVEventAppInit error");
    return JNI_FALSE;
  }

  __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "spawning thread");

  s_appThreadExited = false;
  SetAppFlag(NVEVENT_STATUS_RUNNING);

  /* Launch thread with main loop */
  NVThreadSpawnJNIThread(&s_mainThread, NULL, NVEventMainLoopThreadFunc, NULL);

  __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "thread spawned");

  ++s_nativeRefCount; // native has been successfully constructed
  return JNI_TRUE;
}

static jboolean onStartNative(JNIEnv* env, jobject thiz)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_START;
  return NVEventInsertBlocking(&ev);
}

static jboolean onRestartNative(JNIEnv* env, jobject thiz)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_RESTART;
  return NVEventInsertBlocking(&ev);
}

static jboolean onResumeNative(JNIEnv* env, jobject thiz)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_RESUME;
  SetAppFlag(NVEVENT_STATUS_ACTIVE);
  return NVEventInsertBlocking(&ev);
}

static jboolean onSurfaceCreatedNative(JNIEnv* env, jobject thiz, int w, int h, int density)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_SURFACE_CREATED;
  ev.m_data.m_size.m_w = w;
  ev.m_data.m_size.m_h = h;
  ev.m_data.m_size.m_density = density;
  if ((w > 0) &&  (h > 0))
    SetAppFlag(NVEVENT_STATUS_HAS_REAL_SURFACE);
  else
    ClearAppFlag(NVEVENT_STATUS_HAS_REAL_SURFACE);
  return NVEventInsertBlocking(&ev);
}

static jboolean onFocusChangedNative(JNIEnv* env, jobject thiz, jboolean focused)
{
  NVEvent ev;
  ev.m_type = (focused == JNI_TRUE) ? NV_EVENT_FOCUS_GAINED : NV_EVENT_FOCUS_LOST;
  if (focused)
    SetAppFlag(NVEVENT_STATUS_FOCUSED);
  else
    ClearAppFlag(NVEVENT_STATUS_FOCUSED);
  return NVEventInsertBlocking(&ev);
}

static jboolean onSurfaceChangedNative(JNIEnv* env, jobject thiz, int w, int h, int density)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_SURFACE_SIZE;
  ev.m_data.m_size.m_w = w;
  ev.m_data.m_size.m_h = h;
  ev.m_data.m_size.m_density = density;
  if (w * h)
    SetAppFlag(NVEVENT_STATUS_HAS_REAL_SURFACE);
  else
    ClearAppFlag(NVEVENT_STATUS_HAS_REAL_SURFACE);
  return NVEventInsertBlocking(&ev);
}

static jboolean onSurfaceDestroyedNative(JNIEnv* env, jobject thiz)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_SURFACE_DESTROYED;
  ClearAppFlag(NVEVENT_STATUS_HAS_REAL_SURFACE);
  return NVEventInsertBlocking(&ev);
}

static jboolean onPauseNative(JNIEnv* env, jobject thiz)
{
  // TODO: we could selectively flush here to
  //       improve responsiveness to the pause
  s_eventQueue.Flush();
  NVEvent ev;
  ev.m_type = NV_EVENT_PAUSE;
  ClearAppFlag(NVEVENT_STATUS_ACTIVE);
  return NVEventInsertBlocking(&ev);
}

static jboolean onStopNative(JNIEnv* env, jobject thiz)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_STOP;
  return NVEventInsertBlocking(&ev);
}

static jboolean onDestroyNative(JNIEnv* env, jobject thiz)
{
  std::lock_guard<std::mutex> const l(s_nativeConstructionMutex);

  if (0 == s_nativeRefCount)
  {
    __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: native has not been created");
    return JNI_FALSE;
  }

  if ((--s_nativeRefCount) != 0)
  {
    __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Native still has reference");
    return JNI_TRUE;
  }

  if (!env || !s_globalThiz)
  {
    __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Error: DestroyingRegisteredObjectInstance no TLS data!");
  }

  if (!s_appThreadExited)
  {
    __android_log_print(ANDROID_LOG_DEBUG, MODULE, "Posting quit event");

    // flush ALL events
    s_eventQueue.Flush();

    NVEvent ev;
    ev.m_type = NV_EVENT_QUIT;
    ClearAppFlag(NVEVENT_STATUS_RUNNING);

    // We're posting quit, so we need to mark that; when the main loop
    // thread exits, we check this flag to ensure that we only call "finish"
    // if the app returned of its own accord, not if we posted it
    s_javaPostedQuit = true;
    NVEventInsert(&ev);

    // ensure that the native side
    // isn't blocked waiting for an event -- since we've flushed
    // all the events save quit, we must artificially unblock native
    s_eventQueue.UnblockConsumer();

    __android_log_print(ANDROID_LOG_DEBUG, MODULE, "Waiting for main loop exit");
    pthread_join(s_mainThread, NULL);
    __android_log_print(ANDROID_LOG_DEBUG, MODULE, "Main loop exited");
  }

  env->DeleteGlobalRef(s_globalThiz);
  s_globalThiz = NULL;

  __android_log_print(ANDROID_LOG_DEBUG, MODULE,  "Released global thiz!");

  s_eventQueue.Shutdown();

  return JNI_TRUE;
}

static jboolean postUserEvent(JNIEnv* env, jobject thiz,
							  jint u0, jint u1, jint u2, jint u3,
							  jboolean blocking)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_USER;
  ev.m_data.m_user.m_u0 = u0;
  ev.m_data.m_user.m_u1 = u1;
  ev.m_data.m_user.m_u2 = u2;
  ev.m_data.m_user.m_u3 = u3;
  if (blocking == JNI_TRUE)
  {
    return NVEventInsertBlocking(&ev);
  }
  else
  {
    NVEventInsert(&ev);
    return true;
  }
}

void postMWMEvent(void * pFn, bool blocking)
{
  NVEvent ev;
  ev.m_type = NV_EVENT_MWM;
  ev.m_data.m_mwm.m_pFn = pFn;

  if (blocking)
    NVEventInsertBlocking(&ev);
  else
    NVEventInsert(&ev);
}

static std::mutex s_renderFrameMutex; // protects renderFrame members
static bool s_renderFrameValid = false;
static RenderFrameRequest s_renderFrame;

void postRenderFrameRequest(double x, double y, double scale, size_t width, size_t height)
{
  std::lock_guard<std::mutex> const l(s_renderFrameMutex);
  s_renderFrame.m_x = x;
  s_renderFrame.m_y = y;
  s_renderFrame.m_scale = scale;
  s_renderFrame.m_width = width;
  s_renderFrame.m_height = height;
  s_renderFrameValid = true;
}

bool getRenderFrameRequest(RenderFrameRequest & info)
{
  std::lock_guard<std::mutex> const l(s_renderFrameMutex);
  if (s_renderFrameValid)
  {
    s_renderFrameValid = false;
    info = s_renderFrame;
    return true;
  }
  else
    return false;
}

///////////////////////////////////////////////////////////////////////////////
// File and APK handling functions

char* NVEventLoadFile(const char* file)
{
  __android_log_print(ANDROID_LOG_DEBUG, MODULE, "loadFile is not implemented");
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// JVM Initialization functions

void InitNVEvent(JavaVM* vm)
{
  JNIEnv* env = nullptr;

  NVThreadInit(vm);

  NVDEBUG("InitNVEvent called");

  if (vm->GetEnv((void**)&env, JNI_VERSION_1_4) != JNI_OK)
  {
    NVDEBUG("Failed to get the environment using GetEnv()");
    return;
  }

  JNINativeMethod methods[] =
  {
    {
      "onCreateNative",
      "()Z",
      (void *) onCreateNative
    },
    {
      "onStartNative",
      "()Z",
      (void *) onStartNative
    },
    {
      "onRestartNative",
      "()Z",
      (void *) onRestartNative
    },
    {
      "onResumeNative",
      "()Z",
      (void *) onResumeNative
    },
    {
      "onSurfaceCreatedNative",
      "(III)Z",
      (void *) onSurfaceCreatedNative
    },
    {
      "onFocusChangedNative",
      "(Z)Z",
      (void *) onFocusChangedNative
    },
    {
      "onSurfaceChangedNative",
      "(III)Z",
      (void *) onSurfaceChangedNative
    },
    {
      "onSurfaceDestroyedNative",
      "()Z",
      (void *) onSurfaceDestroyedNative
    },
    {
      "onPauseNative",
      "()Z",
      (void *) onPauseNative
    },
    {
      "onStopNative",
      "()Z",
      (void *) onStopNative
    },
    {
      "onDestroyNative",
      "()Z",
      (void *) onDestroyNative
    },
    {
      "multiTouchEvent",
      "(IZZIIIILandroid/view/MotionEvent;)Z",
      (void *) NVEventMultiTouchEvent
    }/*,
    {
     "onLongClickNative",
     "(II)Z",
     (void *) NVEventLongClickEvent
    }*/
  };

  jclass const k = (env)->FindClass ("com/nvidia/devtech/NvEventQueueFragment");
  (env)->RegisterNatives(k, methods, dimof(methods));

  s_InitEGL.QueryID(env, k);
  s_CleanupEGL.QueryID(env, k);
  s_CreateSurfaceEGL.QueryID(env, k);
  s_CreateOffScreenSurfaceEGL.QueryID(env, k);
  s_DestroySurfaceEGL.QueryID(env, k);
  s_SwapBuffersEGL.QueryID(env, k);
  s_BindSurfaceAndContextEGL.QueryID(env, k);
  s_UnbindSurfaceAndContextEGL.QueryID(env, k);
  s_GetErrorEGL.QueryID(env, k);
  s_finish.QueryID(env, k);
  s_ReportUnsupported.QueryID(env, k);
  s_OnRenderingInitialized.QueryID(env, k);
}

