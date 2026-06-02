#include "SoftKeyboard.h"

#include "Debug.h"

#include <android_native_app_glue.h>
#include <jni.h>

namespace
{
jclass FindSoftKeyboardHelperClass(JNIEnv* env, jobject activity)
{
    jclass helperClass = env->FindClass("com/ixtreeme/client/SoftKeyboardHelper");
    if (helperClass)
        return helperClass;

    env->ExceptionClear();
    Tracen("[KEYBOARD] FindClass failed; trying Activity class loader");

    jclass activityClass = env->GetObjectClass(activity);
    if (!activityClass)
        return nullptr;

    jmethodID getClassLoader = env->GetMethodID(
        activityClass,
        "getClassLoader",
        "()Ljava/lang/ClassLoader;");
    if (!getClassLoader)
    {
        env->ExceptionClear();
        env->DeleteLocalRef(activityClass);
        return nullptr;
    }

    jobject classLoader = env->CallObjectMethod(activity, getClassLoader);
    if (env->ExceptionCheck() || !classLoader)
    {
        env->ExceptionClear();
        env->DeleteLocalRef(activityClass);
        return nullptr;
    }

    jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
    if (!classLoaderClass)
    {
        env->ExceptionClear();
        env->DeleteLocalRef(classLoader);
        env->DeleteLocalRef(activityClass);
        return nullptr;
    }

    jmethodID loadClass = env->GetMethodID(
        classLoaderClass,
        "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClass)
    {
        env->ExceptionClear();
        env->DeleteLocalRef(classLoaderClass);
        env->DeleteLocalRef(classLoader);
        env->DeleteLocalRef(activityClass);
        return nullptr;
    }

    jstring className = env->NewStringUTF("com.ixtreeme.client.SoftKeyboardHelper");
    jobject loadedClass = env->CallObjectMethod(classLoader, loadClass, className);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        loadedClass = nullptr;
    }

    env->DeleteLocalRef(className);
    env->DeleteLocalRef(classLoaderClass);
    env->DeleteLocalRef(classLoader);
    env->DeleteLocalRef(activityClass);
    return static_cast<jclass>(loadedClass);
}

void CallSoftKeyboardMethod(android_app* app, const char* methodName)
{
    if (!app || !app->activity || !app->activity->vm || !app->activity->clazz)
    {
        Tracenf("[KEYBOARD] %s skipped: NativeActivity is not available", methodName);
        return;
    }

    JavaVM* jvm = app->activity->vm;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            Tracenf("[KEYBOARD] AttachCurrentThread failed for %s", methodName);
            return;
        }
        detach = true;
    }

    jclass helperClass = FindSoftKeyboardHelperClass(env, app->activity->clazz);
    if (!helperClass)
    {
        Tracenf("[KEYBOARD] FindClass(SoftKeyboardHelper) failed for %s", methodName);
        if (detach)
            jvm->DetachCurrentThread();
        return;
    }

    jmethodID methodId = env->GetStaticMethodID(
        helperClass,
        methodName,
        "(Landroid/app/NativeActivity;)V");
    if (!methodId)
    {
        env->ExceptionClear();
        Tracenf("[KEYBOARD] GetStaticMethodID(%s) failed", methodName);
        env->DeleteLocalRef(helperClass);
        if (detach)
            jvm->DetachCurrentThread();
        return;
    }

    env->CallStaticVoidMethod(helperClass, methodId, app->activity->clazz);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        Tracenf("[KEYBOARD] %s threw a Java exception", methodName);
    }
    else
    {
        Tracenf("[KEYBOARD] %s called", methodName);
    }

    env->DeleteLocalRef(helperClass);
    if (detach)
        jvm->DetachCurrentThread();
}
}

void ShowSoftKeyboard(android_app* app)
{
    CallSoftKeyboardMethod(app, "showSoftKeyboard");
}

void HideSoftKeyboard(android_app* app)
{
    CallSoftKeyboardMethod(app, "hideSoftKeyboard");
}
