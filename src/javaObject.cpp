
#include "javaObject.h"
#include "java.h"
#include "utils.h"
#include <sstream>

/*static*/ v8::Persistent<v8::FunctionTemplate> JavaObject::s_ct;

/*static*/ void JavaObject::Init(v8::Handle<v8::Object> target) {
  v8::HandleScope scope;

  v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New();
  s_ct = v8::Persistent<v8::FunctionTemplate>::New(t);
  s_ct->InstanceTemplate()->SetInternalFieldCount(1);
  s_ct->SetClassName(v8::String::NewSymbol("JavaObject"));

  target->Set(v8::String::NewSymbol("JavaObject"), s_ct->GetFunction());
}

/*static*/ v8::Local<v8::Object> JavaObject::New(Java *java, jobject obj) {
  v8::HandleScope scope;
  v8::Local<v8::Function> ctor = s_ct->GetFunction();
  v8::Local<v8::Object> javaObjectObj = ctor->NewInstance();
  JavaObject *self = new JavaObject(java, obj);
  self->Wrap(javaObjectObj);

  JNIEnv *env = self->m_java->getJavaEnv();

  std::list<jobject> methods = javaReflectionGetMethods(env, self->m_class);
  jclass methodClazz = env->FindClass("java/lang/reflect/Method");
  jmethodID method_getName = env->GetMethodID(methodClazz, "getName", "()Ljava/lang/String;");
  for(std::list<jobject>::iterator it = methods.begin(); it != methods.end(); it++) {
    std::string methodNameStr = javaToString(env, (jstring)env->CallObjectMethod(*it, method_getName));

    v8::Handle<v8::String> methodName = v8::String::New(methodNameStr.c_str());
    v8::Local<v8::FunctionTemplate> methodCallTemplate = v8::FunctionTemplate::New(methodCall, methodName);
    javaObjectObj->Set(methodName, methodCallTemplate->GetFunction());

    v8::Handle<v8::String> methodNameSync = v8::String::New((methodNameStr + "Sync").c_str());
    v8::Local<v8::FunctionTemplate> methodCallSyncTemplate = v8::FunctionTemplate::New(methodCallSync, methodName);
    javaObjectObj->Set(methodNameSync, methodCallSyncTemplate->GetFunction());
  }

  self->m_fields = javaReflectionGetFields(env, self->m_class);
  jclass fieldClazz = env->FindClass("java/lang/reflect/Field");
  jmethodID field_getName = env->GetMethodID(fieldClazz, "getName", "()Ljava/lang/String;");
  for(std::list<jobject>::iterator it = self->m_fields.begin(); it != self->m_fields.end(); it++) {
    std::string fieldNameStr = javaToString(env, (jstring)env->CallObjectMethod(*it, field_getName));

    v8::Handle<v8::String> fieldName = v8::String::New(fieldNameStr.c_str());
    javaObjectObj->SetAccessor(fieldName, fieldGetter, fieldSetter);
  }

  return scope.Close(javaObjectObj);
}

JavaObject::JavaObject(Java *java, jobject obj) {
  m_java = java;
  m_obj = m_java->getJavaEnv()->NewGlobalRef(obj);
  m_class = m_java->getJavaEnv()->GetObjectClass(obj);
}

JavaObject::~JavaObject() {
  JNIEnv *env = m_java->getJavaEnv();

  jclass nodeDynamicProxyClass = env->FindClass("node/NodeDynamicProxyClass");
  if(env->IsInstanceOf(m_obj, nodeDynamicProxyClass)) {
    jfieldID ptrField = env->GetFieldID(nodeDynamicProxyClass, "ptr", "J");
    DynamicProxyData* proxyData = (DynamicProxyData*)(long)env->GetLongField(m_obj, ptrField);
    if(dynamicProxyDataVerify(proxyData)) {
      delete proxyData;
    }
  }

  m_java->getJavaEnv()->DeleteGlobalRef(m_obj);
}

/*static*/ v8::Handle<v8::Value> JavaObject::methodCall(const v8::Arguments& args) {
  v8::HandleScope scope;
  JavaObject* self = node::ObjectWrap::Unwrap<JavaObject>(args.This());
  JNIEnv *env = self->m_java->getJavaEnv();

  v8::String::AsciiValue methodName(args.Data());
  std::string methodNameStr = *methodName;

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_BACK_CALLBACK();

  if(!callbackProvided && methodNameStr == "toString") {
    return methodCallSync(args);
  }

  jobjectArray methodArgs = v8ToJava(env, args, argsStart, argsEnd);

  jobject method = javaFindMethod(env, self->m_class, methodNameStr, methodArgs);
  if(method == NULL) {
    EXCEPTION_CALL_CALLBACK("Could not find method " << methodNameStr);
    return v8::Undefined();
  }

  // run
  InstanceMethodCallBaton* baton = new InstanceMethodCallBaton(self->m_java, self, method, methodArgs, callback);
  baton->run();

  END_CALLBACK_FUNCTION("\"Method '" << methodNameStr << "' called without a callback did you mean to use the Sync version?\"");
}

/*static*/ v8::Handle<v8::Value> JavaObject::methodCallSync(const v8::Arguments& args) {
  v8::HandleScope scope;
  JavaObject* self = node::ObjectWrap::Unwrap<JavaObject>(args.This());
  JNIEnv *env = self->m_java->getJavaEnv();

  v8::String::AsciiValue methodName(args.Data());
  std::string methodNameStr = *methodName;

  int argsStart = 0;
  int argsEnd = args.Length();

  jobjectArray methodArgs = v8ToJava(env, args, argsStart, argsEnd);

  jobject method = javaFindMethod(env, self->m_class, methodNameStr, methodArgs);
  if(method == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find method " << methodNameStr;
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // run
  v8::Handle<v8::Value> callback = v8::Object::New();
  InstanceMethodCallBaton* baton = new InstanceMethodCallBaton(self->m_java, self, method, methodArgs, callback);
  v8::Handle<v8::Value> result = baton->runSync();
  delete baton;
  return scope.Close(result);;
}

/*static*/ v8::Handle<v8::Value> JavaObject::fieldGetter(v8::Local<v8::String> property, const v8::AccessorInfo& info) {
  v8::HandleScope scope;
  JavaObject* self = node::ObjectWrap::Unwrap<JavaObject>(info.This());
  JNIEnv *env = self->m_java->getJavaEnv();

  v8::String::AsciiValue propertyCStr(property);
  std::string propertyStr = *propertyCStr;
  jobject field = javaFindField(env, self->m_class, propertyStr);
  if(field == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find field " << propertyStr;
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  jclass fieldClazz = env->FindClass("java/lang/reflect/Field");
  jmethodID field_get = env->GetMethodID(fieldClazz, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");

  // get field value
  jobject val = env->CallObjectMethod(field, field_get, self->m_obj);
  if(env->ExceptionOccurred()) {
    std::ostringstream errStr;
    errStr << "Could not get field " << propertyStr;
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  return scope.Close(javaToV8(self->m_java, env, val));
}

/*static*/ void JavaObject::fieldSetter(v8::Local<v8::String> property, v8::Local<v8::Value> value, const v8::AccessorInfo& info) {
  v8::HandleScope scope;
  JavaObject* self = node::ObjectWrap::Unwrap<JavaObject>(info.This());
  JNIEnv *env = self->m_java->getJavaEnv();

  jobject newValue = v8ToJava(env, value);

  v8::String::AsciiValue propertyCStr(property);
  std::string propertyStr = *propertyCStr;
  jobject field = javaFindField(env, self->m_class, propertyStr);
  if(field == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find field " << propertyStr;
    ThrowException(javaExceptionToV8(env, errStr.str()));
    return;
  }

  jclass fieldClazz = env->FindClass("java/lang/reflect/Field");
  jmethodID field_set = env->GetMethodID(fieldClazz, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V");

  //printf("newValue: %s\n", javaObjectToString(env, newValue).c_str());

  // set field value
  env->CallObjectMethod(field, field_set, self->m_obj, newValue);
  if(env->ExceptionOccurred()) {
    std::ostringstream errStr;
    errStr << "Could not set field " << propertyStr;
    ThrowException(javaExceptionToV8(env, errStr.str()));
    return;
  }
}
