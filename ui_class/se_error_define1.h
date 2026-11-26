#ifndef SE_ERROR_DEFINE_H
#define SE_ERROR_DEFINE_H

// 错误代码定义
typedef int SE_Error;

#define SE_ERROR_NONE 0
#define SE_ERROR_INVALID_PARAMETER 1
#define SE_ERROR_FILE_NOT_FOUND 2
#define SE_ERROR_WRITE_FILE_FAILED 3
#define SE_ERROR_CORRUPT_DATA 4
#define SE_ERROR_FAILURE 5
#define SE_ERROR_MEMORY_ALLOCATION_FAILED 6
#define SE_ERROR_INVALID_DATA_FORMAT 7
#define SE_ERROR_OPERATION_TIMEOUT 8
#define SE_ERROR_NETWORK_ERROR 9
#define SE_ERROR_DATABASE_ERROR 10
#define SE_ERROR_PERMISSION_DENIED 11
#define SE_ERROR_NOT_IMPLEMENTED 12

// 错误代码描述 - 移除中文字符，使用英文
static const char* SE_ErrorToString(SE_Error error) {
    switch (error) {
    case SE_ERROR_NONE: return "Success";
    case SE_ERROR_INVALID_PARAMETER: return "Invalid parameter";
    case SE_ERROR_FILE_NOT_FOUND: return "File not found";
    case SE_ERROR_WRITE_FILE_FAILED: return "Write file failed";
    case SE_ERROR_CORRUPT_DATA: return "Corrupt data";
    case SE_ERROR_FAILURE: return "Operation failed";
    case SE_ERROR_MEMORY_ALLOCATION_FAILED: return "Memory allocation failed";
    case SE_ERROR_INVALID_DATA_FORMAT: return "Invalid data format";
    case SE_ERROR_OPERATION_TIMEOUT: return "Operation timeout";
    case SE_ERROR_NETWORK_ERROR: return "Network error";
    case SE_ERROR_DATABASE_ERROR: return "Database error";
    case SE_ERROR_PERMISSION_DENIED: return "Permission denied";
    case SE_ERROR_NOT_IMPLEMENTED: return "Not implemented";
    default: return "Unknown error";
    }
}

#endif // SE_ERROR_DEFINE_H