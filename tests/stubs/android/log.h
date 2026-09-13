// Host-only stubs so tree_feller.cpp compiles outside the NDK. The shipped
// mod never uses these files — the NDK provides the real <android/log.h>.
#pragma once
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
int __android_log_print(int prio, const char* tag, const char* fmt, ...);