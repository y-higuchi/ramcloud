/* Copyright (c) 2013 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <RamCloud.h>
#include <TableEnumerator.h>
#include <Object.h>
#include "edu_stanford_ramcloud_JRamCloud.h"
#include "edu_stanford_ramcloud_JRamCloud_TableEnumerator.h"

using namespace RAMCloud;

/// Our JRamCloud java library is packaged under "edu.stanford.ramcloud".
/// We will need this when using FindClass, etc.
#define PACKAGE_PATH "edu/stanford/ramcloud/"

#define check_null(var, msg)                                                \
    if (var == NULL) {                                                      \
        throw Exception(HERE, "JRamCloud: NULL returned: " msg "\n");       \
    }

/**
 * This class provides a simple means of extracting C-style strings
 * from a jstring and cleans up when the destructor is called. This
 * avoids having to manually do the annoying GetStringUTFChars /
 * ReleaseStringUTFChars dance. 
 */
class JStringGetter {
  public:
    JStringGetter(JNIEnv* env, jstring jString)
        : env(env)
        , jString(jString)
        , string(env->GetStringUTFChars(jString, 0))
    {
        check_null(string, "GetStringUTFChars failed");
    }
    
    ~JStringGetter()
    {
        if (string != NULL)
            env->ReleaseStringUTFChars(jString, string);
    }

  private:    
    JNIEnv* env;
    jstring jString;

  public:
    const char* const string;
};

/**
 * This class provides a simple means of accessing jbyteArrays as
 * C-style void* buffers and cleans up when the destructor is called.
 * This avoids having to manually do the annoying GetByteArrayElements /
 * ReleaseByteArrayElements dance.
 */
class JByteArrayGetter {
  public:
    JByteArrayGetter(JNIEnv* env, jbyteArray jByteArray)
        : env(env)
        , jByteArray(jByteArray)
        , pointer(static_cast<void*>(env->GetByteArrayElements(jByteArray, 0)))
        , length(env->GetArrayLength(jByteArray))
    {
        check_null(pointer, "GetByteArrayElements failed");
    }
    
    ~JByteArrayGetter()
    {
        if (pointer != NULL) {
            env->ReleaseByteArrayElements(jByteArray,
                                          reinterpret_cast<jbyte*>(pointer),
                                          0);
        }
    }

  private:    
    JNIEnv* env;
    jbyteArray jByteArray;

  public:
    void* const pointer;
    const jsize length;
};

static RamCloud*
getRamCloud(JNIEnv* env, jobject jRamCloud)
{
    jclass cls = env->GetObjectClass(jRamCloud);
    jfieldID fieldId = env->GetFieldID(cls, "ramcloudObjectPointer", "J");
    return reinterpret_cast<RamCloud*>(env->GetLongField(jRamCloud, fieldId));
}

static TableEnumerator*
getTableEnumerator(JNIEnv* env, jobject jTableEnumerator)
{
    jclass cls = env->GetObjectClass(jTableEnumerator);
    jfieldID fieldId = env->GetFieldID(cls, "tableEnumeratorObjectPointer", "J");
    return reinterpret_cast<TableEnumerator*>(env->GetLongField(jTableEnumerator, fieldId));    
}

static void
createException(JNIEnv* env, jobject jRamCloud, const char* name)
{
    // Need to specify the full class name, including the package. To make it
    // slightly more complicated, our exceptions are nested under the JRamCloud
    // class.
    string fullName = PACKAGE_PATH;
    fullName += "JRamCloud$";
    fullName += name;

    // This would be much easier if we didn't make our Exception classes nested
    // under JRamCloud since env->ThrowNew() could be used instead. The problem
    // is that ThrowNew assumes a particular method signature that happens to
    // be incompatible with the nested classes' signatures.
    jclass cls = env->FindClass(fullName.c_str());
    check_null(cls, "FindClass failed");

    jmethodID methodId = env->GetMethodID(cls,
                                          "<init>",
                                          "(L" PACKAGE_PATH "JRamCloud;Ljava/lang/String;)V");
    check_null(methodId, "GetMethodID failed");

    jstring jString = env->NewStringUTF("");
    check_null(jString, "NewStringUTF failed");

    jthrowable exception = reinterpret_cast<jthrowable>(
        env->NewObject(cls, methodId, jRamCloud, jString));
    check_null(exception, "NewObject failed");

    env->Throw(exception);
}

/**
 * This macro is used to catch C++ exceptions and convert them into Java
 * exceptions. Be sure to wrap the individual RamCloud:: calls in try blocks,
 * rather than the entire methods, since doing so with functions that return
 * non-void is a bad idea with undefined(?) behaviour. 
 *
 * _returnValue is the value that should be returned from the JNI function
 * when an exception is caught and generated in Java. As far as I can tell,
 * the exception fires immediately upon returning from the JNI method. I
 * don't think anything else would make sense, but the JNI docs kind of
 * suck.
 */
#define EXCEPTION_CATCHER(_returnValue)                                        \
    catch (TableDoesntExistException& e) {                                     \
        createException(env, jRamCloud, "TableDoesntExistException");          \
        return _returnValue;                                                   \
    } catch (ObjectDoesntExistException& e) {                                  \
        createException(env, jRamCloud, "ObjectDoesntExistException");         \
        return _returnValue;                                                   \
    } catch (ObjectExistsException& e) {                                       \
        createException(env, jRamCloud, "ObjectExistsException");              \
        return _returnValue;                                                   \
    } catch (WrongVersionException& e) {                                       \
        createException(env, jRamCloud, "WrongVersionException");              \
        return _returnValue;                                                   \
    }

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    connect
 * Signature: (Ljava/lang/String;)J
 */
JNIEXPORT jlong 
JNICALL Java_edu_stanford_ramcloud_JRamCloud_connect(JNIEnv *env,
                               jclass jRamCloud,
                               jstring coordinatorLocator)
{
    JStringGetter locator(env, coordinatorLocator);
    RamCloud* ramcloud = NULL;
    try {
        ramcloud = new RamCloud(locator.string);
    } EXCEPTION_CATCHER(NULL);
    return reinterpret_cast<jlong>(ramcloud);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    disconnect
 * Signature: (J)V
 */
JNIEXPORT void
JNICALL Java_edu_stanford_ramcloud_JRamCloud_disconnect(JNIEnv *env,
                                  jclass jRamCloud,
                                  jlong ramcloudObjectPointer)
{
    delete reinterpret_cast<RamCloud*>(ramcloudObjectPointer);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    createTable
 * Signature: (Ljava/lang/String;)I
 */
JNIEXPORT jlong
JNICALL Java_edu_stanford_ramcloud_JRamCloud_createTable__Ljava_lang_String_2(JNIEnv *env,
                                                        jobject jRamCloud,
                                                        jstring jTableName)
{
    return Java_edu_stanford_ramcloud_JRamCloud_createTable__Ljava_lang_String_2I(env,
                                                            jRamCloud,
                                                            jTableName,
                                                            1);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    createTable
 * Signature: (Ljava/lang/String;I)I
 */
JNIEXPORT jlong
JNICALL Java_edu_stanford_ramcloud_JRamCloud_createTable__Ljava_lang_String_2I(JNIEnv *env,
                                                         jobject jRamCloud,
                                                         jstring jTableName,
                                                         jint jServerSpan)
{
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JStringGetter tableName(env, jTableName);
    uint64_t tableId;
    try {
        tableId = ramcloud->createTable(tableName.string, jServerSpan);
    } EXCEPTION_CATCHER(-1);
    return static_cast<jlong>(tableId);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    dropTable
 * Signature: (Ljava/lang/String;)I
 */
JNIEXPORT void
JNICALL Java_edu_stanford_ramcloud_JRamCloud_dropTable(JNIEnv *env,
                                 jobject jRamCloud,
                                 jstring jTableName)
{
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JStringGetter tableName(env, jTableName);
    try {
        ramcloud->dropTable(tableName.string);
    } EXCEPTION_CATCHER();
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    getTableId
 * Signature: (Ljava/lang/String;)J
 */
JNIEXPORT jlong
JNICALL Java_edu_stanford_ramcloud_JRamCloud_getTableId(JNIEnv *env,
                                  jobject jRamCloud,
                                  jstring jTableName)
{
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JStringGetter tableName(env, jTableName);
    uint64_t tableId;
    try {
        tableId = ramcloud->getTableId(tableName.string);
    } EXCEPTION_CATCHER(-1);
    return tableId;
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    read
 * Signature: (J[B)LJRamCloud/Object;
 */
JNIEXPORT jobject
JNICALL Java_edu_stanford_ramcloud_JRamCloud_read__J_3B(JNIEnv *env,
                                  jobject jRamCloud,
                                  jlong jTableId,
                                  jbyteArray jKey)
{
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JByteArrayGetter key(env, jKey);

    Buffer buffer;
    uint64_t version;
    try {
        ramcloud->read(jTableId, key.pointer, key.length, &buffer, NULL, &version);
    } EXCEPTION_CATCHER(NULL);

    jbyteArray jValue = env->NewByteArray(buffer.getTotalLength());
    check_null(jValue, "NewByteArray failed");
    JByteArrayGetter value(env, jValue);
    buffer.copy(0, buffer.getTotalLength(), value.pointer);

    // Note that using 'javap -s' on the class file will print out the method
    // signatures (the third argument to GetMethodID).
    jclass cls = env->FindClass(PACKAGE_PATH "JRamCloud$Object");
    check_null(cls, "FindClass failed");

    jmethodID methodId = env->GetMethodID(cls,
                                          "<init>",
                                          "(L" PACKAGE_PATH "JRamCloud;[B[BJ)V");
    check_null(methodId, "GetMethodID failed");

    return env->NewObject(cls,
                          methodId,
                          jRamCloud,
                          jKey,
                          jValue,
                          static_cast<jlong>(version));
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    read
 * Signature: (J[BLJRamCloud/RejectRules;)LJRamCloud/Object;
 */
JNIEXPORT jobject
JNICALL Java_edu_stanford_ramcloud_JRamCloud_read__J_3BLJRamCloud_RejectRules_2(JNIEnv *env,
                                                          jobject jRamCloud,
                                                          jlong jTableId,
                                                          jbyteArray jKey,
                                                          jobject jRejectRules)
{
    // XXX-- implement me by generalising the other read() method.
    return NULL;
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    multiRead
 * Signature: ([Ledu/stanford/ramcloud/JRamCloud$multiReadObject;I)[Ledu/stanford/ramcloud/JRamCloud$Object;
 */
JNIEXPORT jobjectArray
JNICALL Java_edu_stanford_ramcloud_JRamCloud_multiRead(JNIEnv *env,   
                                                        jobject jRamCloud,
                                                        jobjectArray jmultiReadArray){

    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    jint requestNum = env->GetArrayLength(jmultiReadArray);
    MultiReadObject objects[requestNum];
    Tub<Buffer> values[requestNum];
    jbyteArray jKey[requestNum];

    for (int i = 0 ; i < requestNum ; i++){
        jobject obj = env->GetObjectArrayElement(jmultiReadArray, i);
        check_null(obj, "GetObjectArrayElement failed");
        jclass cls = env->GetObjectClass(obj);

        jfieldID fid = env->GetFieldID(cls, "tableId", "J");
        jlong jTableId = env->GetLongField(obj, fid);

        fid = env->GetFieldID(cls, "key", "[B");
        jKey[i] = (jbyteArray)env->GetObjectField (obj, fid);

        jbyte* data = env->GetByteArrayElements(jKey[i], NULL);
        check_null(data, "GetByteArrayElements failed");

        objects[i].tableId = jTableId;
        objects[i].key = data;
        objects[i].keyLength = env->GetArrayLength(jKey[i]);
        objects[i].value = &values[i];
    }

    MultiReadObject* requests[requestNum];
    for (int i = 0 ; i < requestNum ; i++) {
    	requests[i] = &objects[i];
    }

    try {
        ramcloud->multiRead(requests, requestNum);
    } EXCEPTION_CATCHER(NULL);

    jclass cls = env->FindClass(PACKAGE_PATH "JRamCloud$Object");
    check_null(cls, "FindClass failed");
    jmethodID methodId = env->GetMethodID(cls,
                                        "<init>",
                                        "(L" PACKAGE_PATH "JRamCloud;[B[BJ)V");

    jobjectArray outJNIArray = env->NewObjectArray(requestNum, cls , NULL);
    check_null(outJNIArray, "NewObjectArray failed");
    
    for (int i = 0 ; i < requestNum ; i++) {
        jbyteArray jValue = env->NewByteArray(values[i].get()->getTotalLength());
        check_null(jValue, "NewByteArray failed");
        JByteArrayGetter value(env, jValue);
        values[i].get()->copy(0, values[i].get()->getTotalLength(), value.pointer);
        jobject obj = env->NewObject(cls, methodId, jRamCloud, jKey[i], jValue);
        check_null(obj, "NewObject failed");
        env->SetObjectArrayElement(outJNIArray, i, obj);
        env->ReleaseByteArrayElements(jKey[i], (jbyte *)objects[i].key, 0);
    }
    return outJNIArray;
}


/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    remove
 * Signature: (J[B)J
 */
JNIEXPORT jlong
JNICALL Java_edu_stanford_ramcloud_JRamCloud_remove__J_3B(JNIEnv *env,
                                    jobject jRamCloud,
                                    jlong jTableId,
                                    jbyteArray jKey)
{
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JByteArrayGetter key(env, jKey);
    uint64_t version;
    try {
        ramcloud->remove(jTableId, key.pointer, key.length, NULL, &version);
    } EXCEPTION_CATCHER(-1);
    return static_cast<jlong>(version);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    remove
 * Signature: (J[BLJRamCloud/RejectRules;)J
 */
JNIEXPORT jlong
JNICALL Java_edu_stanford_ramcloud_JRamCloud_remove__J_3BLJRamCloud_RejectRules_2(JNIEnv *env,
                                                           jobject jRamCloud,
                                                           jlong jTableId,
                                                           jbyteArray jKey,
                                                           jobject jRejectRules)
{
    // XXX- handle RejectRules
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JByteArrayGetter key(env, jKey);
    uint64_t version;
    try {
        ramcloud->remove(jTableId, key.pointer, key.length, NULL, &version);
    } EXCEPTION_CATCHER(-1);
    return static_cast<jlong>(version);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    write
 * Signature: (J[B[B)J
 */
JNIEXPORT jlong
JNICALL Java_edu_stanford_ramcloud_JRamCloud_write__J_3B_3B(JNIEnv *env,
                                      jobject jRamCloud,
                                      jlong jTableId,
                                      jbyteArray jKey,
                                      jbyteArray jValue)
{
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JByteArrayGetter key(env, jKey);
    JByteArrayGetter value(env, jValue);
    uint64_t version;
    try {
        ramcloud->write(jTableId,
                        key.pointer, key.length,
                        value.pointer, value.length,
                        NULL,
                        &version);
    } EXCEPTION_CATCHER(-1);
    return static_cast<jlong>(version);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud
 * Method:    write
 * Signature: (J[B[BLJRamCloud/RejectRules;)J
 */
JNIEXPORT jlong
JNICALL Java_edu_stanford_ramcloud_JRamCloud_write__J_3B_3BLJRamCloud_RejectRules_2(JNIEnv *env,
                                                           jobject jRamCloud,
                                                           jlong jTableId,
                                                           jbyteArray jKey,
                                                           jbyteArray jValue,
                                                           jobject jRejectRules)
{
    // XXX- handle RejectRules
    RamCloud* ramcloud = getRamCloud(env, jRamCloud);
    JByteArrayGetter key(env, jKey);
    JByteArrayGetter value(env, jValue);
    uint64_t version;
    try {
        ramcloud->write(jTableId,
                        key.pointer, key.length,
                        value.pointer, value.length,
                        NULL,
                        &version);
    } EXCEPTION_CATCHER(-1);
    return static_cast<jlong>(version);
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud_TableEnumerator
 * Method:    init
 * Signature: (J)V
 */
JNIEXPORT jlong JNICALL Java_edu_stanford_ramcloud_JRamCloud_00024TableEnumerator_init(JNIEnv *env, 
                                                                                      jobject jTableEnumerator, 
                                                                                      jlong jTableId)
{
    jclass cls = env->GetObjectClass(jTableEnumerator);
    jfieldID fieldId = env->GetFieldID(cls, "ramCloudObjectPointer", "J");
    RamCloud* ramcloud = reinterpret_cast<RamCloud*>(env->GetLongField(jTableEnumerator, fieldId));
  
    return reinterpret_cast<jlong>(new TableEnumerator(*ramcloud, jTableId));
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud_TableEnumerator
 * Method:    hasNext
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL Java_edu_stanford_ramcloud_JRamCloud_00024TableEnumerator_hasNext( JNIEnv *env, 
                                                                                              jobject jTableEnumerator)
{
    TableEnumerator* tableEnum = getTableEnumerator(env, jTableEnumerator);
    return static_cast<jboolean>(tableEnum->hasNext());
}

/*
 * Class:     edu_stanford_ramcloud_JRamCloud_TableEnumerator
 * Method:    next
 * Signature: ()Ledu/stanford/ramcloud/JRamCloud/Object;
 */
JNIEXPORT jobject JNICALL Java_edu_stanford_ramcloud_JRamCloud_00024TableEnumerator_next( JNIEnv *env, 
                                                                                          jobject jTableEnumerator)
{
    TableEnumerator* tableEnum = getTableEnumerator(env, jTableEnumerator);

    if(tableEnum->hasNext()) 
    {
        uint32_t size = 0;
        const void* buffer = 0;
        uint64_t version = 0;

        tableEnum->next(&size, &buffer);
        Object object(buffer, size);

        jbyteArray jKey = env->NewByteArray(object.getKeyLength());
        jbyteArray jValue = env->NewByteArray(object.getDataLength());
        
        JByteArrayGetter key(env, jKey);
        JByteArrayGetter value(env, jValue);

        memcpy(key.pointer, object.getKey(), object.getKeyLength());
        memcpy(value.pointer, object.getData(), object.getDataLength());

        version = object.getVersion();
        
        jclass cls = env->FindClass(PACKAGE_PATH "JRamCloud$Object");
        check_null(cls, "FindClass failed");
        jmethodID methodId = env->GetMethodID(cls,
                                          "<init>",
                                          "(L" PACKAGE_PATH "JRamCloud;[B[BJ)V");
        check_null(methodId, "GetMethodID failed");
        return env->NewObject(cls,
                              methodId,
                              jTableEnumerator,
                              jKey,
                              jValue,
                              static_cast<jlong>(version));        
    } else 
        return NULL;
}
