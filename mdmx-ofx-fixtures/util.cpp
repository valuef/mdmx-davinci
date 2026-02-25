// Copyright (C) 2026 ValueFactory https://value.gay

#include <cstdarg>

struct Read_File_Bin {
  std::shared_ptr<void> data;
  size_t size;
  const char* err;
};

// TODO break out from MAX_PATH
static
Read_File_Bin
read_entire_file_bin(
  const wchar_t* path
) {
  Read_File_Bin ret = {};

  auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if (handle == INVALID_HANDLE_VALUE) {
    ret.err = "Failed to open file.";
    return ret;
  }
  defer{ CloseHandle(handle); };

  auto size = GetFileSize(handle, 0);
  if (size == 0) {
    ret.err = "File is empty.";
    return ret;
  }
  else if (size == INVALID_FILE_SIZE) {
    ret.err = "Failed to get file size.";
    return ret;
  }
  auto buf = std::shared_ptr<char>(new char[size]);

  DWORD read_bytes = 0;
  auto read_result = ReadFile(handle, buf.get(), size, &read_bytes, 0);
  if (read_result != TRUE) {
    ret.err = "ReadFile failed.";
    return ret;
  }

  ret.data = buf;
  ret.size = read_bytes;

  return ret;
}

static
bool
read_entire_file_string(
  const wchar_t* path,
  std::string& str,
  std::string& err
) {
  auto read = read_entire_file_bin(path);
  if (read.err) {
    err = read.err;
    return false;
  }

  str = std::string((char*)read.data.get(), read.size);
  return true;
}

// TODO break out from MAX_PATH
static
bool
write_file(
  const wchar_t *path, 
  const void *data, 
  size_t num_bytes,
  bool overwrite = false
) {
  DWORD behavriour = CREATE_NEW;
  if(overwrite) {
    behavriour = CREATE_ALWAYS;
  }

  auto handle = CreateFileW(path, GENERIC_WRITE, 0, 0, behavriour, FILE_ATTRIBUTE_NORMAL, 0);
  if(handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  defer{ CloseHandle(handle); };

  DWORD num_bytes_written = 0;
  auto result = WriteFile(handle, data, num_bytes, &num_bytes_written, 0);
  if (result != TRUE) {
    return false;
  }

  return true;
}

static
std::string
wstr_to_str(
  const wchar_t* wc
) {
  if(wc == 0) {
    return std::string();
  }

  auto size_needed = WideCharToMultiByte(CP_UTF8, 0, wc, -1, nullptr, 0, nullptr, nullptr);

  if(size_needed <= 0) {
    return std::string();
  }

  std::string utf8_string(size_needed - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wc, -1, &utf8_string[0], size_needed, nullptr, nullptr);

  return utf8_string;
}

static
std::wstring
str_to_wstr(
  const char* str
) {
  if(str == nullptr) {
    return std::wstring();
  }

  auto size_needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);

  if (size_needed <= 0) {
    return std::wstring();
  }

  std::wstring wide_string(size_needed - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str, -1, &wide_string[0], size_needed);

  return wide_string;
}

static
std::string
str_fmt(const char *fmt ...) {
  va_list args_count;
  va_start(args_count, fmt);

  va_list args_fmt;
  va_copy(args_fmt, args_count);

  auto bytes_needed = vsnprintf(0, 0, fmt, args_count);
  va_end(args_count);

  auto* mem = (char*)malloc(bytes_needed + 1);
  defer{ free(mem); };

  vsnprintf(mem, bytes_needed + 1, fmt, args_fmt);
  va_end(args_fmt);

  return std::string(mem, bytes_needed);
}

static
std::string
time_fmt(const tm *time, const char* fmt) {
  const int NUM_BYTES = 1024;
  char buf[NUM_BYTES];
  auto bytes_written = strftime(buf, sizeof(buf), fmt, time);

  if (bytes_written <= 0) return "";

  if (bytes_written >= NUM_BYTES) {
    bytes_written = NUM_BYTES - 1;
  }
 
  return std::string(buf, bytes_written);
}


static
bool
read_entire_file_string(
  std::string& path,
  std::string& str,
  std::string& err
) {
  auto wstr = str_to_wstr(path.c_str());
  return read_entire_file_string(wstr.c_str(), str, err);
}

static
std::string
get_filename(
  std::string& str
) {
  auto idx = str.find_last_of('\\');
  if (idx == std::string::npos) {
    return str;
  }

  idx += 1;
  if (str.length() <= idx) {
    return str;
  }

  return str.substr(idx);
}

static
bool
str_equal(
  const char *a,
  const char *b
) {
  return strcmp(a,b) == 0;
}

static
std::wstring
get_temp_dir() {
  wchar_t dir_buf[MAX_PATH + 1];
  auto ret = GetTempPathW(ARRAYSIZE(dir_buf), dir_buf);

  if(ret > MAX_PATH || ret == 0) {
    return std::wstring(L"C:\\");
  }

  return std::wstring(dir_buf);
}
