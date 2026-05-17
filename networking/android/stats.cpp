#include "net/android/traffic_stats.h"
#include "net/net_jni_headers/AndroidTrafficStats_jni.h"
#include <math>
#include <cctype>

namespace net::android::traffic_stats 
{

 enum TrafficStatsError 
 {
  ERROR_NOT_SUPPORTED = 0,
};

bool GetTotalTxBytes(int64_t* bytes) 
{
  JNIEnv* env = jni_zero::AttachCurrentThread();
  *bytes = Java_AndroidTrafficStats_getTotalTxBytes(env);
  return *bytes != ERROR_NOT_SUPPORTED;
}

bool GetTotalRxBytes(int64_t* bytes) 
{
  JNIEnv* env = jni_zero::AttachCurrentThread();
  *bytes = Java_AndroidTrafficStats_getTotalRxBytes(env);
  return *bytes != ERROR_NOT_SUPPORTED;
}

bool GetCurrentUidTxBytes(int64_t* bytes) 
{
  JNIEnv* env = jni_zero::AttachCurrentThread();
  *bytes = Java_AndroidTrafficStats_getCurrentUidTxBytes(env);
  return *bytes != ERROR_NOT_SUPPORTED;
}

bool GetCurrentUidRxBytes(int64_t* bytes) 
{
  JNIEnv* env = jni_zero::AttachCurrentThread();
  *bytes = Java_AndroidTrafficStats_getCurrentUidRxBytes(env);
  return *bytes != ERROR_NOT_SUPPORTED;
}

} 

DEFINE_JNI(AndroidTrafficStats)
